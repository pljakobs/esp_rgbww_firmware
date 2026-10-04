#!/usr/bin/env bash
#
# testswarm-virtual.sh — scale-test mDNS neighbour discovery for ONE real
# controller against synthetic advertisers.
#
# Unlike testswarm-local.sh (which launches N real Host emulators on a bridge so
# they discover each other), this harness launches a SINGLE device-under-test
# (DUT) emulator on one TAP and uses mdns_swarm_check.py / virtual_controllers.py
# to advertise a growing fleet of synthetic Lightinator controllers. It verifies
# the DUT converges on 16, then 32, 48 and 64 ONLINE (state==3) neighbours within
# a bounded time per stage.
#
# Layout: tap0 host side 192.168.13.1 (mDNS source), DUT 192.168.13.2, synthetic
# controllers 192.168.13.100+. The Python checker is the only HTTP client against
# the DUT, so it never contends with heavy endpoints.
#
# SOFT by default (reports but exits 0). Set SWARM_STRICT=1 to fail on
# non-convergence. Requires root (or CAP_NET_ADMIN) and /dev/net/tun.
#
#   sudo ./testswarm-virtual.sh [mode]
#   MDNS_STAGES=16,32 sudo ./testswarm-virtual.sh none
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=testlib.sh
source "$SCRIPT_DIR/testlib.sh"

MODE="${1:-${TESTNET_MODE:-none}}"
SWARM_STRICT="${SWARM_STRICT:-0}"
SWARM_SKIP_BUILD="${SWARM_SKIP_BUILD:-0}"

TAP_IF="$TL_TAP_IF"
HOST_CIDR="$TL_HOST_CIDR"
HOST_IP="$TL_HOST_IP"
APP_IP="$TL_APP_IP"
NETMASK="$TL_NETMASK"
FLASH_SIZE="$TL_FLASH_SIZE"

READY_TIMEOUT="${SWARM_READY_TIMEOUT:-90}"
PYTHON_BIN="${MDNS_PYTHON:-python3}"

REPO_ROOT="$SCRIPT_DIR"
RUN_DIR="$REPO_ROOT/out/Host/debug"
APP_BIN="$RUN_DIR/firmware/app"
FLASH_TEMPLATE="$RUN_DIR/firmware/flash.bin"
WORK_DIR="$REPO_ROOT/out/host-swarm"
FLASH_BIN="$WORK_DIR/dut-flash.bin"
APP_LOG="$WORK_DIR/dut.log"
SUMMARY_FILE="$WORK_DIR/summary.txt"

log()  { echo "[vswarm] $*"; }
warn() { echo "[vswarm][warn] $*" >&2; }
err()  { echo "[vswarm][error] $*" >&2; }

SUDO=""
if [[ "$(id -u)" -ne 0 ]]; then
  command -v sudo >/dev/null 2>&1 && SUDO="sudo"
fi

IP_BIN="$(command -v ip || echo /sbin/ip)"
APP_PID=""

cleanup() {
  set +e
  log "Tearing down..."
  if [[ -n "$APP_PID" ]] && kill -0 "$APP_PID" 2>/dev/null; then
    kill "$APP_PID" 2>/dev/null
    wait "$APP_PID" 2>/dev/null
  fi
  if $IP_BIN link show "$TAP_IF" >/dev/null 2>&1; then
    $SUDO $IP_BIN link set "$TAP_IF" down 2>/dev/null
    $SUDO $IP_BIN tuntap del dev "$TAP_IF" mode tap 2>/dev/null
  fi
}
trap cleanup EXIT

mkdir -p "$WORK_DIR"
: > "$SUMMARY_FILE"

# ---------------------------------------------------------------------------
# Build the DUT (unless reusing an existing build)
# ---------------------------------------------------------------------------
tl_source_sming
if [[ "$SWARM_SKIP_BUILD" != "1" ]]; then
  log "Building Host DUT binary..."
  tl_build "$REPO_ROOT" 0
fi
if [[ ! -x "$APP_BIN" ]]; then
  err "DUT binary not found at $APP_BIN (build first or unset SWARM_SKIP_BUILD)."
  echo "result: FAIL (no DUT binary)" > "$SUMMARY_FILE"
  exit 1
fi

# Use a private flash copy so the DUT starts from the freshly built ConfigDB and
# never mutates the build template between runs.
cp -f "$FLASH_TEMPLATE" "$FLASH_BIN"

# ---------------------------------------------------------------------------
# Network: single isolated TAP, host side .1, DUT .2
# ---------------------------------------------------------------------------
log "Setting up TAP $TAP_IF ($HOST_IP host / $APP_IP DUT)..."
$IP_BIN link show "$TAP_IF" >/dev/null 2>&1 && $SUDO $IP_BIN link del "$TAP_IF"
$SUDO $IP_BIN tuntap add dev "$TAP_IF" mode tap user "$(id -un)"
$SUDO $IP_BIN addr add "$HOST_CIDR" dev "$TAP_IF"
$SUDO $IP_BIN link set "$TAP_IF" up

# ---------------------------------------------------------------------------
# Launch the single DUT in the background
# ---------------------------------------------------------------------------
log "Launching DUT emulator ($MODE)..."
(
  cd "$RUN_DIR" || exit 1
  exec "$APP_BIN" \
    --flashfile="$FLASH_BIN" \
    --flashsize="$FLASH_SIZE" \
    --ifname="$TAP_IF" \
    --ipaddr="$APP_IP" \
    --gateway="$HOST_IP" \
    --netmask="$NETMASK"
) >"$APP_LOG" 2>&1 &
APP_PID=$!

# ---------------------------------------------------------------------------
# Wait for the DUT HTTP API to come up
# ---------------------------------------------------------------------------
log "Waiting up to ${READY_TIMEOUT}s for DUT HTTP API at http://$APP_IP ..."
ready=0
for ((t = 0; t < READY_TIMEOUT; t++)); do
  if ! kill -0 "$APP_PID" 2>/dev/null; then
    err "DUT process exited during startup; see $APP_LOG"
    break
  fi
  if curl -fsS --max-time 3 "http://$APP_IP/hosts" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 1
done
if (( ready != 1 )); then
  err "DUT HTTP API did not come up within ${READY_TIMEOUT}s."
  echo "result: FAIL (DUT not ready)" > "$SUMMARY_FILE"
  [[ "$SWARM_STRICT" == "1" ]] && exit 1
  exit 0
fi
log "DUT is up. Starting staged mDNS neighbour-scaling check."

# ---------------------------------------------------------------------------
# Run the staged checker (handles 16/32/48/64, convergence, summary)
# ---------------------------------------------------------------------------
export MDNS_BASE_URL="http://$APP_IP"
export MDNS_INTERFACE_IP="$HOST_IP"
export MDNS_REPORT="$SUMMARY_FILE"
export MDNS_STRICT="$SWARM_STRICT"

rc=0
"$PYTHON_BIN" "$REPO_ROOT/mdns_swarm_check.py" || rc=$?

log "Checker exited with code $rc."
if (( rc != 0 )) && [[ "$SWARM_STRICT" == "1" ]]; then
  exit "$rc"
fi
exit 0
