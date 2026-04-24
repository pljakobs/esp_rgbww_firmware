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
WS_HOST="${WS_HOST:-$APP_IP}"
WS_PORT="${WS_PORT:-80}"
WS_PATH="${WS_PATH:-/ws}"

cleanup() {
  set +e
  if [[ -n "${APP_PID:-}" ]] && kill -0 "$APP_PID" 2>/dev/null; then
    kill "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
  fi
  if [[ -n "${IP_BIN:-}" ]] && "$IP_BIN" link show "$TAP_IF" >/dev/null 2>&1; then
    "$IP_BIN" link del "$TAP_IF" >/dev/null 2>&1 || true
  fi
}

trap cleanup EXIT

resolve_ip_bin() {
  if command -v ip >/dev/null 2>&1; then
    command -v ip
    return 0
  fi

  for path in /sbin/ip /usr/sbin/ip /bin/ip /usr/bin/ip; do
    if [[ -x "$path" ]]; then
      echo "$path"
      return 0
    fi
  done

  return 1
}

ensure_ip_tool() {
  if IP_BIN="$(resolve_ip_bin)"; then
    return 0
  fi

  echo "ip command not found; attempting to install iproute tools" >&2

  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -y >/dev/null
    DEBIAN_FRONTEND=noninteractive apt-get install -y iproute2 >/dev/null
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y iproute >/dev/null
  elif command -v yum >/dev/null 2>&1; then
    yum install -y iproute >/dev/null
  elif command -v apk >/dev/null 2>&1; then
    apk add --no-cache iproute2 >/dev/null
  else
    echo "No supported package manager found to install ip command" >&2
    return 1
  fi

  if ! IP_BIN="$(resolve_ip_bin)"; then
    echo "ip command still unavailable after installation attempt" >&2
    return 1
  fi
}

mkdir -p "$LOG_DIR"
rm -f "$APP_LOG" "$LOG_DIR/info.json"

if ! ensure_ip_tool; then
  echo "cannot configure TAP interface without ip command" >&2
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

# Sming's export.sh references SMING_HOME directly and is not nounset-safe.
set +u
source "$export_script"
set -u

make configdb-rebuild
make SMING_ARCH=Host flash DISABLE_WERROR=1 COM_SPEED=115200

FLASH_BIN="${FIRMWARE_DIR}/flash.bin"
PARTITIONS_BIN="${FIRMWARE_DIR}/partitions.bin"
HOST_CONFIG_MK="out/Host/debug/config.mk"

if [[ ! -f "$PARTITIONS_BIN" ]]; then
  echo "Missing Host partition table: $PARTITIONS_BIN" >&2
  exit 1
fi

if [[ ! -f "$HOST_CONFIG_MK" ]]; then
  echo "Missing Host build config: $HOST_CONFIG_MK" >&2
  exit 1
fi

PARTITION_OFFSET_HEX="$(awk -F= '/^PARTITION_TABLE_OFFSET=/{print $2; exit}' "$HOST_CONFIG_MK")"
if [[ -z "$PARTITION_OFFSET_HEX" ]]; then
  echo "Could not determine PARTITION_TABLE_OFFSET from $HOST_CONFIG_MK" >&2
  exit 1
fi

# Ensure flash.bin exists and contains partitions.bin at PARTITION_TABLE_OFFSET.
python3 - "$FLASH_BIN" "$PARTITIONS_BIN" "$PARTITION_OFFSET_HEX" <<'PY'
import os
import sys

flash_path = sys.argv[1]
partitions_path = sys.argv[2]
partition_offset = int(sys.argv[3], 0)

with open(partitions_path, "rb") as f:
    partitions = f.read()

if not partitions:
    raise SystemExit(f"Partition table is empty: {partitions_path}")

flash_size = 4 * 1024 * 1024
required_size = max(flash_size, partition_offset + len(partitions))

if not os.path.exists(flash_path):
    with open(flash_path, "wb") as f:
        f.truncate(required_size)

with open(flash_path, "r+b") as f:
    current_size = os.path.getsize(flash_path)
    if current_size < required_size:
        f.truncate(required_size)
    f.seek(partition_offset)
    current = f.read(len(partitions))
    if current != partitions:
        f.seek(partition_offset)
        f.write(partitions)
        f.flush()
        os.fsync(f.fileno())
        print(f"Injected partition table into {flash_path} at 0x{partition_offset:x}")
    else:
        print(f"Partition table already present in {flash_path} at 0x{partition_offset:x}")
PY

if "$IP_BIN" link show "$TAP_IF" >/dev/null 2>&1; then
  "$IP_BIN" link del "$TAP_IF"
fi

"$IP_BIN" tuntap add dev "$TAP_IF" mode tap user "$(id -un)"
"$IP_BIN" addr add "$HOST_CIDR" dev "$TAP_IF"
"$IP_BIN" link set "$TAP_IF" up

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

python3 - "$WS_HOST" "$WS_PORT" "$WS_PATH" <<'PY'
import base64
import hashlib
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
path = sys.argv[3]

nonce = base64.b64encode(b"host-ci-websocket-key").decode("ascii")
request = (
  f"GET {path} HTTP/1.1\r\n"
  f"Host: {host}:{port}\r\n"
  "Upgrade: websocket\r\n"
  "Connection: Upgrade\r\n"
  f"Sec-WebSocket-Key: {nonce}\r\n"
  "Sec-WebSocket-Version: 13\r\n"
  "\r\n"
)

with socket.create_connection((host, port), timeout=5) as sock:
  sock.sendall(request.encode("ascii"))
  response = sock.recv(4096).decode("latin-1", errors="replace")

status_line = response.split("\r\n", 1)[0].strip()
if "101" not in status_line:
  raise SystemExit(f"WebSocket handshake failed: {status_line}\n{response}")

headers = {}
for line in response.split("\r\n")[1:]:
  if not line:
    break
  if ":" in line:
    k, v = line.split(":", 1)
    headers[k.strip().lower()] = v.strip()

expected_accept = base64.b64encode(
  hashlib.sha1((nonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
).decode("ascii")

accept = headers.get("sec-websocket-accept", "")
if accept != expected_accept:
  raise SystemExit(
    f"Invalid Sec-WebSocket-Accept header: {accept!r} != {expected_accept!r}"
  )

print(f"WebSocket handshake OK: {status_line}")
PY

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