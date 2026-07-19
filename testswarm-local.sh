#!/usr/bin/env bash
#
# testswarm-local.sh — launch a swarm of Host-emulator controllers on ONE Linux
# bridge and check whether they discover each other via mDNS.
#
# Unlike testswarm.sh (one emulator per podman container, NAT'd onto a shared
# podman network), this harness runs N emulator processes directly on the host,
# each attached to its own TAP interface, with every TAP enslaved to a single
# Linux bridge. That puts all instances in one L2 broadcast domain so mDNS
# multicast (224.0.0.251:5353) floods between them exactly like real devices on
# a switch — no NAT, no per-instance container.
#
# Each instance gets:
#   * its own TAP (swtap<i>) with a unique MAC, enslaved to the bridge,
#   * a unique IP (SUBNET.PREFIX.<BASE_OCTET + i>),
#   * a unique chip id via LI_CHIP_ID (see app/host_identity.cpp) so the mDNS
#     hostname / TXT id / MQTT id differ — otherwise the swarm collapses into a
#     single logical node,
#   * its own copy of flash.bin (ConfigDB) so writes never collide.
#
# Discovery is asserted purely over HTTP: each controller exposes the number of
# ONLINE peers it has learned as `neighbours` in GET /info (self is state
# LOCALHOST and is NOT counted), so full convergence for a swarm of N is
# neighbours == N-1 on every instance.
#
# The neighbour-count check is SOFT by default: results are reported but the
# script still exits 0 (set SWARM_STRICT=1 to make non-convergence fail).
#
# Requires: root (or CAP_NET_ADMIN) for bridge/TAP setup, and /dev/net/tun.
#
#   sudo ./testswarm-local.sh [mode]
#   SWARM_SIZE=5 sudo ./testswarm-local.sh none
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=testlib.sh
source "$SCRIPT_DIR/testlib.sh"

# ---------------------------------------------------------------------------
# Configuration (all overridable via environment)
# ---------------------------------------------------------------------------
SWARM_SIZE="${SWARM_SIZE:-10}"
MODE="${1:-${TESTNET_MODE:-none}}"
SWARM_STRICT="${SWARM_STRICT:-0}"           # 1 => non-convergence exits non-zero
SWARM_SKIP_BUILD="${SWARM_SKIP_BUILD:-0}"   # 1 => reuse an existing Host build

BRIDGE="${SWARM_BRIDGE:-br-swarm}"
SUBNET_PREFIX="${SWARM_SUBNET_PREFIX:-192.168.13}"
GATEWAY_IP="${SWARM_GATEWAY_IP:-${SUBNET_PREFIX}.1}"
BASE_OCTET="${SWARM_BASE_OCTET:-10}"        # first controller at .10
CHIP_BASE="${SWARM_CHIP_BASE:-0x00CAFE00}"  # LI_CHIP_ID of instance 0
NETMASK="${TL_NETMASK:-255.255.255.0}"
FLASH_SIZE="${TL_FLASH_SIZE:-4M}"

READY_TIMEOUT="${SWARM_READY_TIMEOUT:-60}"      # secs to wait for HTTP to come up
CONVERGE_TIMEOUT="${SWARM_CONVERGE_TIMEOUT:-120}" # secs to wait for mDNS convergence
POLL_INTERVAL="${SWARM_POLL_INTERVAL:-3}"

REPO_ROOT="$SCRIPT_DIR"
RUN_DIR="$REPO_ROOT/out/Host/debug"
APP_BIN="$RUN_DIR/firmware/app"
FLASH_TEMPLATE="$RUN_DIR/firmware/flash.bin"
WORK_DIR="$REPO_ROOT/out/host-swarm"
SUMMARY_FILE="$WORK_DIR/summary.txt"

IP_BIN=""
declare -a APP_PIDS=()
declare -a TAPS=()

log()  { echo "[swarm] $*"; }
warn() { echo "[swarm][warn] $*" >&2; }
err()  { echo "[swarm][error] $*" >&2; }

# ---------------------------------------------------------------------------
# Teardown
# ---------------------------------------------------------------------------
cleanup() {
  set +e
  log "Tearing down swarm..."
  for pid in "${APP_PIDS[@]:-}"; do
    [[ -n "$pid" ]] || continue
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
    fi
  done
  if [[ -n "$IP_BIN" ]]; then
    for tap in "${TAPS[@]:-}"; do
      [[ -n "$tap" ]] || continue
      "$IP_BIN" link del "$tap" 2>/dev/null
    done
    "$IP_BIN" link show "$BRIDGE" >/dev/null 2>&1 && "$IP_BIN" link del "$BRIDGE" 2>/dev/null
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------
resolve_ip_bin() {
  command -v ip 2>/dev/null && return 0
  for p in /sbin/ip /usr/sbin/ip /bin/ip /usr/bin/ip; do
    [[ -x "$p" ]] && { echo "$p"; return 0; }
  done
  return 1
}

require_tools() {
  if ! IP_BIN="$(resolve_ip_bin)"; then
    err "'ip' command not found (install iproute2)."
    exit 1
  fi
  if ! command -v curl >/dev/null 2>&1; then
    err "'curl' not found."
    exit 1
  fi
}

# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------
ip_for()   { echo "${SUBNET_PREFIX}.$((BASE_OCTET + $1))"; }
info_url() { echo "http://$(ip_for "$1")/info"; }

http_ok() {  # http_ok <ip>
  curl -s -o /dev/null --max-time 3 "http://$1/info"
}

neighbours_of() {  # neighbours_of <ip> -> prints integer or "-"
  local body
  body="$(curl -s --max-time 3 "http://$1/info" 2>/dev/null)" || { echo "-"; return; }
  local n
  n="$(printf '%s' "$body" | grep -oE '"neighbours"[[:space:]]*:[[:space:]]*[0-9]+' | grep -oE '[0-9]+' | head -n1)"
  [[ -n "$n" ]] && echo "$n" || echo "-"
}

# ---------------------------------------------------------------------------
# Build (unless reusing an existing binary)
# ---------------------------------------------------------------------------
maybe_build() {
  if [[ "$SWARM_SKIP_BUILD" == "1" ]]; then
    log "SWARM_SKIP_BUILD=1 — reusing existing Host build."
    return 0
  fi
  log "Building Host emulator (SMING_ARCH=Host)..."
  tl_source_sming
  ( cd "$REPO_ROOT" && tl_build "$REPO_ROOT" 0 )
}

# ---------------------------------------------------------------------------
# Network fabric: one bridge, N enslaved TAPs
# ---------------------------------------------------------------------------
setup_bridge() {
  log "Creating bridge $BRIDGE ($GATEWAY_IP/24)..."
  "$IP_BIN" link show "$BRIDGE" >/dev/null 2>&1 && "$IP_BIN" link del "$BRIDGE" 2>/dev/null
  "$IP_BIN" link add name "$BRIDGE" type bridge
  # Flood mDNS multicast to every port (no IGMP querier on this synthetic LAN).
  "$IP_BIN" link set "$BRIDGE" type bridge mcast_snooping 0 2>/dev/null || \
    { [[ -w "/sys/class/net/$BRIDGE/bridge/multicast_snooping" ]] && \
        echo 0 > "/sys/class/net/$BRIDGE/bridge/multicast_snooping"; }
  "$IP_BIN" addr add "$GATEWAY_IP/24" dev "$BRIDGE"
  "$IP_BIN" link set "$BRIDGE" up
}

setup_tap() {  # setup_tap <index>
  local i="$1" tap; tap="swtap${i}"
  local mac; mac="$(printf '02:00:00:00:00:%02x' "$((i + 1))")"
  "$IP_BIN" tuntap add dev "$tap" mode tap 2>/dev/null || true
  "$IP_BIN" link set "$tap" address "$mac" 2>/dev/null || true
  "$IP_BIN" link set "$tap" master "$BRIDGE"
  "$IP_BIN" link set "$tap" up
  TAPS+=("$tap")
  echo "$tap"
}

# ---------------------------------------------------------------------------
# Launch one emulator instance
# ---------------------------------------------------------------------------
launch_instance() {  # launch_instance <index> <tap>
  local i="$1" tap="$2"
  local inst_dir="$WORK_DIR/$i"
  local flash="$inst_dir/flash.bin"
  local app_log="$inst_dir/app.log"
  local ip; ip="$(ip_for "$i")"
  local chip; chip="$(printf '0x%08x' "$((CHIP_BASE + i))")"

  mkdir -p "$inst_dir"
  if [[ -f "$FLASH_TEMPLATE" ]]; then
    cp -f "$FLASH_TEMPLATE" "$flash"
  else
    warn "flash template $FLASH_TEMPLATE missing; instance $i will create a fresh image."
    flash="$inst_dir/flash.bin"
  fi

  log "Instance $i: ip=$ip chip=$chip tap=$tap"
  (
    cd "$inst_dir"
    LI_CHIP_ID="$chip" "$APP_BIN" \
      --flashfile="$flash" \
      --flashsize="$FLASH_SIZE" \
      --ifname="$tap" \
      --ipaddr="$ip" \
      --gateway="$GATEWAY_IP" \
      --netmask="$NETMASK"
  ) >"$app_log" 2>&1 &
  APP_PIDS+=("$!")
}

# ---------------------------------------------------------------------------
# Wait for all instances to serve HTTP
# ---------------------------------------------------------------------------
wait_ready() {
  log "Waiting for HTTP readiness (timeout ${READY_TIMEOUT}s)..."
  local deadline=$((SECONDS + READY_TIMEOUT))
  local ready=0
  while (( SECONDS < deadline )); do
    ready=0
    for ((i = 0; i < SWARM_SIZE; i++)); do
      http_ok "$(ip_for "$i")" && ready=$((ready + 1))
    done
    log "  ready: $ready/$SWARM_SIZE"
    (( ready == SWARM_SIZE )) && return 0
    sleep "$POLL_INTERVAL"
  done
  warn "Only $ready/$SWARM_SIZE instances became reachable within ${READY_TIMEOUT}s."
  return 1
}

# ---------------------------------------------------------------------------
# Poll for mDNS convergence (soft)
# ---------------------------------------------------------------------------
wait_converge() {
  local expected=$((SWARM_SIZE - 1))
  log "Waiting for mDNS convergence (expect neighbours=$expected, timeout ${CONVERGE_TIMEOUT}s)..."
  local deadline=$((SECONDS + CONVERGE_TIMEOUT))
  local converged=0
  while (( SECONDS < deadline )); do
    converged=0
    local line=""
    for ((i = 0; i < SWARM_SIZE; i++)); do
      local n; n="$(neighbours_of "$(ip_for "$i")")"
      line+=" $i=$n"
      [[ "$n" == "$expected" ]] && converged=$((converged + 1))
    done
    log "  converged: $converged/$SWARM_SIZE   [$line ]"
    (( converged == SWARM_SIZE )) && return 0
    sleep "$POLL_INTERVAL"
  done
  return 1
}

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
report() {  # report <converged_all 0|1>
  local all="$1"
  local expected=$((SWARM_SIZE - 1))
  mkdir -p "$WORK_DIR"
  {
    echo "# Multi-controller mDNS discovery swarm"
    echo "size=$SWARM_SIZE mode=$MODE expected_neighbours=$expected"
    echo
    printf '%-6s %-16s %-12s %-8s\n' "inst" "ip" "neighbours" "status"
    local full=0
    for ((i = 0; i < SWARM_SIZE; i++)); do
      local ip n st
      ip="$(ip_for "$i")"
      n="$(neighbours_of "$ip")"
      if [[ "$n" == "$expected" ]]; then st="OK"; full=$((full + 1))
      elif [[ "$n" == "-" ]]; then st="UNREACHABLE"
      else st="PARTIAL"; fi
      printf '%-6s %-16s %-12s %-8s\n' "$i" "$ip" "$n" "$st"
    done
    echo
    echo "fully_converged=$full/$SWARM_SIZE"
    if (( all )); then
      echo "result=PASS (all controllers see all peers)"
    else
      echo "result=SOFT-FAIL (not all controllers fully converged) — reported, not fatal"
    fi
  } | tee "$SUMMARY_FILE"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
  if ! tl_validate_mode "$MODE"; then
    exit 2
  fi
  if (( SWARM_SIZE < 2 )); then
    err "SWARM_SIZE must be >= 2 (got $SWARM_SIZE)."
    exit 2
  fi
  if (( SWARM_SIZE > 250 )); then
    err "SWARM_SIZE must be <= 250 (single /24)."
    exit 2
  fi

  require_tools
  mkdir -p "$WORK_DIR"
  maybe_build

  if [[ ! -x "$APP_BIN" ]]; then
    err "Host app binary not found at $APP_BIN (build first, or drop SWARM_SKIP_BUILD)."
    exit 1
  fi

  setup_bridge
  for ((i = 0; i < SWARM_SIZE; i++)); do
    tap="$(setup_tap "$i")"
    launch_instance "$i" "$tap"
  done

  # Fail hard only if nothing came up at all — that's a real breakage, not a
  # discovery shortfall.
  if ! wait_ready; then
    local up=0
    for ((i = 0; i < SWARM_SIZE; i++)); do http_ok "$(ip_for "$i")" && up=$((up + 1)); done
    if (( up == 0 )); then
      err "No controllers became reachable; swarm launch failed."
      report 0
      exit 1
    fi
    warn "Continuing with $up/$SWARM_SIZE reachable controllers."
  fi

  local converged=0
  if wait_converge; then
    converged=1
    log "All $SWARM_SIZE controllers fully discovered their peers."
  else
    warn "mDNS convergence incomplete within ${CONVERGE_TIMEOUT}s (soft check)."
  fi

  report "$converged"

  if (( converged == 0 )) && [[ "$SWARM_STRICT" == "1" ]]; then
    err "SWARM_STRICT=1 and convergence incomplete — failing."
    exit 1
  fi
  # Soft by default: report only.
  exit 0
}

main "$@"
