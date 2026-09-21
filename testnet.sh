#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$REPO_ROOT/testlib.sh"

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
# CLI Argument Parsing
# ---------------------------------------------------------------------------
MODE=""
LOG_FILE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --log)
      LOG_FILE="$2"
      shift 2
      ;;
    --log=*)
      LOG_FILE="${1#*=}"
      shift 1
      ;;
    -*)
      echo "[-] Unknown option: $1" >&2
      exit 2
      ;;
    *)
      if [[ -z "$MODE" ]]; then
        MODE="$1"
      else
        echo "[-] Unexpected positional argument: $1" >&2
        exit 2
      fi
      shift 1
      ;;
  esac
done

MODE="${MODE:-${TESTNET_MODE:-memcheck}}"

tl_validate_mode "$MODE" || exit 2

ENABLE_SANITIZERS_VAL="$(tl_sanitizers_for_mode "$MODE")"

DIAG_DIR="$REPO_ROOT/out/host-diag"
mkdir -p "$DIAG_DIR"
VALGRIND_SUPP="$REPO_ROOT/.github/scripts/valgrind.supp"

echo "[+] Diagnostic mode: $MODE (ENABLE_SANITIZERS=$ENABLE_SANITIZERS_VAL)"

EXT_IF=$(ip route show default | awk '/default/ {print $5}' | head -n1)

if [[ -z "$EXT_IF" ]]; then
  echo "[-] Warning: No default internet route detected. Internet access will likely fail." >&2
fi

cleanup() {
  echo -e "\n[+] Tearing down network interface and NAT routing tables..."
  vgdb leak_check 2>/dev/null || true
  
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

APP_ARGS=(
  --flashfile="$FLASH_BIN"
  --flashsize="$FLASH_SIZE"
  --ifname="$TAP_IF"
  --ipaddr="$APP_IP"
  --gateway="$HOST_IP"
  --netmask="$NETMASK"
)

export LOG_FILE REPO_ROOT
tl_launch "$MODE" "$APP_BIN" "$DIAG_DIR" "$VALGRIND_SUPP" -- "${APP_ARGS[@]}"