#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$REPO_ROOT/testlib.sh"

# Network layout + app paths come from testlib.sh (shared with testswarm.sh).
TAP_IF="$TL_TAP_IF"
HOST_CIDR="$TL_HOST_CIDR"
HOST_IP="$TL_HOST_IP"
APP_IP="$TL_APP_IP"
NETMASK="$TL_NETMASK"

HOST_RUN_DIR="$REPO_ROOT/out/Host/debug"
APP_BIN="$TL_APP_BIN"
FLASH_BIN="$TL_FLASH_BIN"
FLASH_SIZE="$TL_FLASH_SIZE"

# ---------------------------------------------------------------------------
# Diagnostic mode selection
# ---------------------------------------------------------------------------
# Usage:  ./testnet.sh [mode]     (or set TESTNET_MODE=<mode>)
#
#   memcheck  (default) Valgrind memcheck, hardened flags: use-after-free,
#             uninitialised reads, invalid heap access + leak check. Best for
#             HEAP CORRUPTION. Fills freed/allocated blocks and delays reuse so
#             corruption surfaces far from the bug. On-demand snapshots via vgdb.
#   dhat      Valgrind DHAT heap profiler: per-callsite allocation lifetimes,
#             short-lived churn and blocks-alive-at-peak. Best proxy for HEAP
#             FRAGMENTATION drivers (JSON docs / String temporaries).
#   massif    Valgrind Massif: time-series of heap + stack with allocation
#             trees (ms_print). Shows growth and stack depth per call site.
#   asan      Build with ASan+UBSan and run WITHOUT valgrind. Only tool that
#             detects STACK corruption (stack-buffer-overflow, use-after-return,
#             global-overflow). Mutually exclusive with valgrind.
#   gdb       Run under gdbserver (no instrumentation). Waits for a debugger to
#             attach from OUTSIDE, then stops the app in gdb on SIGSEGV/SIGABRT
#             so 'bt' shows the real crash site. Attach with the VS Code launch
#             config "Host: Attach to gdbserver (testnet.sh gdb)" or:
#               gdb out/Host/debug/firmware/app -ex 'target remote :1234' -ex continue
#   none      Run the binary directly, no instrumentation (fast repro).
#
# NOTE: ASan and valgrind cannot be combined. Toggling the sanitizer flag also
# requires a clean rebuild (stale .o keep __asan_*/__ubsan_* refs), which this
# script performs automatically when the sanitizer state changes.
MODE="${1:-${TESTNET_MODE:-memcheck}}"

tl_validate_mode "$MODE" || exit 2

# ASan builds are the only ones that enable sanitizers; every valgrind mode must
# build WITHOUT sanitizers so the instrumented binary is not run under valgrind.
ENABLE_SANITIZERS_VAL="$(tl_sanitizers_for_mode "$MODE")"

# Per-run artifact directory (absolute paths — relative paths break valgrind
# --log-file / --*-out-file after we cd into HOST_RUN_DIR).
DIAG_DIR="$REPO_ROOT/out/host-diag"
mkdir -p "$DIAG_DIR"
VALGRIND_SUPP="$REPO_ROOT/.github/scripts/valgrind.supp"

echo "[+] Diagnostic mode: $MODE (ENABLE_SANITIZERS=$ENABLE_SANITIZERS_VAL)"

# Automatically detect your host's active internet-facing network interface
EXT_IF=$(ip route show default | awk '/default/ {print $5}' | head -n1)

if [[ -z "$EXT_IF" ]]; then
  echo "[-] Warning: No default internet route detected. Internet access will likely fail." >&2
fi

cleanup() {
  echo -e "\n[+] Tearing down network interface and NAT routing tables..."
  
  # Remove the added iptables rules (if an interface was detected)
  if [[ -n "${EXT_IF:-}" ]]; then
    sudo iptables -t nat -D POSTROUTING -o "$EXT_IF" -j MASQUERADE 2>/dev/null || true
    sudo iptables -D FORWARD -i "$TAP_IF" -o "$EXT_IF" -j ACCEPT 2>/dev/null || true
    sudo iptables -D FORWARD -i "$EXT_IF" -o "$TAP_IF" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
  fi

  if ip link show "$TAP_IF" >/dev/null 2>&1; then
    sudo ip link set "$TAP_IF" down || true
    sudo ip tuntap del dev "$TAP_IF" mode tap || true
  fi
}

# Bind cleanup to script exit, interruption, or termination signals
trap cleanup EXIT

echo "[+] Compiling target binaries for Host architecture..."
tl_source_sming
tl_build "$REPO_ROOT" "$ENABLE_SANITIZERS_VAL"

echo "[+] Initializing isolated virtual TAP interface..."
if ip link show "$TAP_IF" >/dev/null 2>&1; then
  sudo ip link del "$TAP_IF"
fi

sudo ip tuntap add dev "$TAP_IF" mode tap user "$(id -un)"
sudo ip addr add "$HOST_CIDR" dev "$TAP_IF"
sudo ip link set "$TAP_IF" up

if [[ -n "$EXT_IF" ]]; then
  echo "[+] Enabling kernel IP forwarding and configuring masquerading on $EXT_IF..."
  sudo sysctl -w net.ipv4.ip_forward=1 >/dev/null
  
  # Configure NAT tables to route traffic out of the virtual subnet
  sudo iptables -t nat -A POSTROUTING -o "$EXT_IF" -j MASQUERADE
  sudo iptables -A FORWARD -i "$TAP_IF" -o "$EXT_IF" -j ACCEPT
  sudo iptables -A FORWARD -i "$EXT_IF" -o "$TAP_IF" -m state --state RELATED,ESTABLISHED -j ACCEPT
fi

if [[ ! -d "$HOST_RUN_DIR" ]]; then
  echo "[-] Error: Host build directory not found at $HOST_RUN_DIR" >&2
  exit 1
fi

echo "[+] Launching target binary ($MODE) with routed internet access."
echo "[+] Press Ctrl+C to terminate execution and clean up system routes."
cd "$HOST_RUN_DIR"

# Application arguments (identical across all modes).
APP_ARGS=(
  --flashfile="$FLASH_BIN"
  --flashsize="$FLASH_SIZE"
  --ifname="$TAP_IF"
  --ipaddr="$APP_IP"
  --gateway="$HOST_IP"
  --netmask="$NETMASK"
)

tl_launch "$MODE" "$APP_BIN" "$DIAG_DIR" "$VALGRIND_SUPP" -- "${APP_ARGS[@]}"

