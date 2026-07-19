#!/usr/bin/env bash
#
# In-container entrypoint for testswarm.sh.
#
# Runs INSIDE the runtime container (docker/testswarm.Containerfile). It:
#   1. creates a TAP interface (tap0) with the gateway IP,
#   2. enables IP forwarding and installs a full 1:1 NAT so that everything
#      arriving on the container's primary interface (eth0) is DNAT'd to the
#      emulated firmware (APP_IP) and return/outbound traffic is masqueraded,
#   3. launches the Host emulator under the selected diagnostic MODE (via the
#      shared tl_launch helper, so instrumentation matches testnet.sh exactly).
#
# The container IP (assigned by podman on the shared bridge network) therefore
# transparently maps to the app: another container can simply talk to this
# container's name/IP on port 80 and reach the emulated webserver.
#
# Requires: --cap-add NET_ADMIN, --device /dev/net/tun.
set -euo pipefail

# Shared helpers (bind-mounted alongside this script).
source "/usr/local/bin/testlib.sh"

MODE="${1:-${TESTNET_MODE:-memcheck}}"

# Network layout (from testlib.sh — identical to testnet.sh / the host side).
TAP_IF="$TL_TAP_IF"
HOST_CIDR="$TL_HOST_CIDR"
HOST_IP="$TL_HOST_IP"
APP_IP="$TL_APP_IP"
NETMASK="$TL_NETMASK"
APP_SUBNET="$TL_APP_SUBNET"

# Paths (bind-mounted from the host by testswarm.sh).
RUN_DIR="/run/host"          # -> $REPO_ROOT/out/Host/debug
DIAG_DIR="/run/diag"         # -> $REPO_ROOT/out/host-diag
VALGRIND_SUPP="/run/valgrind.supp"
APP_BIN="$TL_APP_BIN"
FLASH_BIN="$TL_FLASH_BIN"
FLASH_SIZE="$TL_FLASH_SIZE"

mkdir -p "$DIAG_DIR"

# Primary (bridge) interface as seen inside the container.
EXT_IF="$(ip route show default | awk '/default/ {print $5}' | head -n1)"
EXT_IP="$(ip -4 -o addr show "${EXT_IF:-lo}" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n1)"

echo "[+] Container network: EXT_IF=${EXT_IF:-<none>} EXT_IP=${EXT_IP:-<none>} -> APP_IP=$APP_IP"

# ---------------------------------------------------------------------------
# TAP interface
# ---------------------------------------------------------------------------
echo "[+] Creating TAP interface $TAP_IF ($HOST_CIDR)..."
ip tuntap add dev "$TAP_IF" mode tap 2>/dev/null || true
ip addr add "$HOST_CIDR" dev "$TAP_IF" 2>/dev/null || true
ip link set "$TAP_IF" up

# ---------------------------------------------------------------------------
# Full NAT: container IP <-> emulated firmware
# ---------------------------------------------------------------------------
echo "[+] Enabling IP forwarding and NAT (eth-side <-> app-side)..."
sysctl -w net.ipv4.ip_forward=1 >/dev/null

# Make sure forwarding between the two interfaces is permitted.
iptables -P FORWARD ACCEPT 2>/dev/null || true

if [[ -n "${EXT_IF:-}" ]]; then
  # Inbound: anything hitting the container's bridge interface is 1:1 mapped
  # to the emulated firmware. Only the first packet of each flow traverses the
  # nat table (conntrack handles the rest), so this DNATs all TCP/UDP/ICMP.
  iptables -t nat -A PREROUTING -i "$EXT_IF" -j DNAT --to-destination "$APP_IP"

  # Hairpin: the firmware only knows the gateway (HOST_IP) as its next hop, so
  # masquerade traffic entering the TAP so replies come back through conntrack.
  iptables -t nat -A POSTROUTING -o "$TAP_IF" -j MASQUERADE

  # Outbound: let the firmware reach the outside world through the bridge.
  iptables -t nat -A POSTROUTING -s "$APP_SUBNET" -o "$EXT_IF" -j MASQUERADE

  iptables -A FORWARD -i "$EXT_IF" -o "$TAP_IF" -j ACCEPT
  iptables -A FORWARD -i "$TAP_IF" -o "$EXT_IF" -j ACCEPT
else
  echo "[-] Warning: no default route in container; external reachability disabled." >&2
fi

if [[ ! -d "$RUN_DIR" ]]; then
  echo "[-] Error: run dir $RUN_DIR not mounted (expected out/Host/debug)." >&2
  exit 1
fi
cd "$RUN_DIR"

APP_ARGS=(
  --flashfile="$FLASH_BIN"
  --flashsize="$FLASH_SIZE"
  --ifname="$TAP_IF"
  --ipaddr="$APP_IP"
  --gateway="$HOST_IP"
  --netmask="$NETMASK"
)

echo "[+] Launching Host emulator (mode: $MODE)."
[[ -n "${EXT_IP:-}" ]] && echo "[+] Reach the firmware at http://$EXT_IP/ (or via this container's name on the shared network)."

tl_launch "$MODE" "$APP_BIN" "$DIAG_DIR" "$VALGRIND_SUPP" -- "${APP_ARGS[@]}"
