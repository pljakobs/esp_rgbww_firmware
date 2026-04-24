#!/usr/bin/env bash

set -euo pipefail

TAP_IF="${TAP_IF:-tap0}"
HOST_CIDR="${HOST_CIDR:-192.168.13.1/24}"
HOST_IP="${HOST_IP:-192.168.13.1}"
APP_IP="${APP_IP:-192.168.13.2}"
NETMASK="${NETMASK:-255.255.255.0}"
FIRMWARE_DIR="out/Host/debug/firmware"
LOG_DIR="${HOST_CI_LOG_DIR:-out/host-ci}"
APP_LOG="${LOG_DIR}/host-smoke.log"
PING_URL="http://${APP_IP}/ping"
INFO_URL="http://${APP_IP}/info"

cleanup() {
  set +e
  if [[ -n "${APP_PID:-}" ]] && kill -0 "$APP_PID" 2>/dev/null; then
    kill "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
  fi
  if command -v ip >/dev/null 2>&1 && ip link show "$TAP_IF" >/dev/null 2>&1; then
    ip link del "$TAP_IF" >/dev/null 2>&1 || true
  fi
}

trap cleanup EXIT

mkdir -p "$LOG_DIR"
rm -f "$APP_LOG" "$LOG_DIR/info.json"

if ! command -v ip >/dev/null 2>&1; then
  echo "ip command not found; cannot configure TAP interface" >&2
  exit 1
fi

if [[ ! -c /dev/net/tun ]]; then
  echo "/dev/net/tun is not available inside the Sming container" >&2
  exit 1
fi

if [[ -f /opt/Sming/Tools/export.sh ]]; then
  export_script="/opt/Sming/Tools/export.sh"
elif [[ -f /opt/sming/Tools/export.sh ]]; then
  export_script="/opt/sming/Tools/export.sh"
else
  echo "Unable to locate Sming export.sh" >&2
  exit 1
fi

source "$export_script"

make configdb-rebuild
make SMING_ARCH=Host flash DISABLE_WERROR=1 COM_SPEED=115200

if ip link show "$TAP_IF" >/dev/null 2>&1; then
  ip link del "$TAP_IF"
fi

ip tuntap add dev "$TAP_IF" mode tap user "$(id -un)"
ip addr add "$HOST_CIDR" dev "$TAP_IF"
ip link set "$TAP_IF" up

"${FIRMWARE_DIR}/app" \
  --flashfile="${FIRMWARE_DIR}/flash.bin" \
  --flashsize=4M \
  --ifname="$TAP_IF" \
  --ipaddr="$APP_IP" \
  --gateway="$HOST_IP" \
  --netmask="$NETMASK" \
  >"$APP_LOG" 2>&1 &
APP_PID=$!

ready=0
for attempt in $(seq 1 60); do
  if ! kill -0 "$APP_PID" 2>/dev/null; then
    echo "Host emulator exited before becoming ready" >&2
    tail -n 200 "$APP_LOG" >&2 || true
    exit 1
  fi

  if python3 - "$PING_URL" <<'PY'
import json
import sys
import urllib.request

url = sys.argv[1]
with urllib.request.urlopen(url, timeout=2) as response:
    payload = json.load(response)
if payload.get("ping") != "pong":
    raise SystemExit(1)
PY
  then
    ready=1
    break
  fi

  sleep 1
done

if [[ "$ready" != "1" ]]; then
  echo "Timed out waiting for Host API readiness" >&2
  tail -n 200 "$APP_LOG" >&2 || true
  exit 1
fi

python3 - "$INFO_URL" "$APP_IP" "$LOG_DIR/info.json" <<'PY'
import json
import sys
import urllib.request

url = sys.argv[1]
expected_ip = sys.argv[2]
output_path = sys.argv[3]

with urllib.request.urlopen(url, timeout=5) as response:
    payload = json.load(response)

required_keys = ["git_version", "build_type", "sming", "connection"]
missing_keys = [key for key in required_keys if key not in payload]
if missing_keys:
    raise SystemExit(f"Missing keys in /info response: {', '.join(missing_keys)}")

actual_ip = payload.get("connection", {}).get("ip")
if actual_ip != expected_ip:
    raise SystemExit(f"Unexpected Host IP in /info: {actual_ip!r} != {expected_ip!r}")

with open(output_path, "w", encoding="utf-8") as handle:
    json.dump(payload, handle, indent=2, sort_keys=True)

print(json.dumps({
    "git_version": payload.get("git_version"),
    "build_type": payload.get("build_type"),
    "sming": payload.get("sming"),
    "ip": actual_ip,
}, sort_keys=True))
PY

echo "Host smoke test passed"
tail -n 40 "$APP_LOG"