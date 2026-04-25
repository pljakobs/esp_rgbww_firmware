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
APP_LOG_SMOKE="${LOG_DIR}/host-smoke-app.log"
APP_LOG_RGBWW="${LOG_DIR}/host-rgbww-app.log"
HTTP_TRACE_LOG="${LOG_DIR}/http-probes.jsonl"
MALFORMED_JSON_TRACE="${LOG_DIR}/malformed-color-response.json"
VALGRIND_LOG_SMOKE="${LOG_DIR}/valgrind-smoke.log"
VALGRIND_LOG_RGBWW="${LOG_DIR}/valgrind-rgbww.log"
VALGRIND_OPTIONS="${HOST_CI_VALGRIND_OPTIONS:---tool=memcheck --leak-check=full --show-leak-kinds=all --track-origins=yes --num-callers=25}"
ENABLE_VALGRIND="${HOST_CI_ENABLE_VALGRIND:-1}"
ENFORCE_VALGRIND="${HOST_CI_ENFORCE_VALGRIND:-1}"
PING_URL="http://${APP_IP}/ping"
INFO_URL="http://${APP_IP}/info"
WS_HOST="${WS_HOST:-$APP_IP}"
WS_PORT="${WS_PORT:-80}"
WS_PATH="${WS_PATH:-/ws}"
COLOR_URL="http://${APP_IP}/color"

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

ensure_pytest() {
    if python3 -c 'import pytest' >/dev/null 2>&1; then
        return 0
    fi

    echo "pytest not found; installing it into the container environment" >&2
    python3 -m pip install --quiet pytest
}

ensure_valgrind() {
  if [[ "$ENABLE_VALGRIND" != "1" ]]; then
    return 0
  fi

  if command -v valgrind >/dev/null 2>&1; then
    return 0
  fi

  echo "valgrind not found; attempting to install it into the container environment" >&2

  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -y >/dev/null
    DEBIAN_FRONTEND=noninteractive apt-get install -y valgrind >/dev/null || true
    if ! command -v valgrind >/dev/null 2>&1; then
      dpkg --add-architecture i386 >/dev/null 2>&1 || true
      apt-get update -y >/dev/null
      DEBIAN_FRONTEND=noninteractive apt-get install -y valgrind:i386 >/dev/null || true
    fi
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y valgrind >/dev/null || true
    if ! command -v valgrind >/dev/null 2>&1; then
      dnf install -y valgrind.i686 >/dev/null || true
    fi
  elif command -v yum >/dev/null 2>&1; then
    yum install -y valgrind >/dev/null || true
  elif command -v apk >/dev/null 2>&1; then
    apk add --no-cache valgrind >/dev/null || true
  fi

  if ! command -v valgrind >/dev/null 2>&1; then
    if [[ "$ENFORCE_VALGRIND" == "1" ]]; then
      echo "valgrind installation failed and HOST_CI_ENFORCE_VALGRIND=1" >&2
      return 1
    fi
    echo "WARNING: valgrind unavailable, continuing without valgrind" >&2
  fi
}

start_host_app() {
  local app_log_path="$1"
  local vg_log_path="$2"
  local mode="$3"

  if [[ "$mode" == "smoke" ]]; then
    rm -f "$HTTP_TRACE_LOG" "$MALFORMED_JSON_TRACE"
  fi

  if [[ "$ENABLE_VALGRIND" == "1" ]] && command -v valgrind >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    valgrind $VALGRIND_OPTIONS --log-file="$vg_log_path" \
      "${FIRMWARE_DIR}/app" \
      --flashfile="${FIRMWARE_DIR}/flash.bin" \
      --flashsize=4M \
      --ifname="$TAP_IF" \
      --ipaddr="$APP_IP" \
      --gateway="$HOST_IP" \
      --netmask="$NETMASK" \
      >"$app_log_path" 2>&1 &
  else
    "${FIRMWARE_DIR}/app" \
      --flashfile="${FIRMWARE_DIR}/flash.bin" \
      --flashsize=4M \
      --ifname="$TAP_IF" \
      --ipaddr="$APP_IP" \
      --gateway="$HOST_IP" \
      --netmask="$NETMASK" \
      >"$app_log_path" 2>&1 &
  fi
  APP_PID=$!

  local ready=0
  for attempt in $(seq 1 60); do
    if ! kill -0 "$APP_PID" 2>/dev/null; then
      echo "Host emulator exited before becoming ready" >&2
      tail -n 200 "$app_log_path" >&2 || true
      if [[ -f "$vg_log_path" ]]; then
        tail -n 200 "$vg_log_path" >&2 || true
      fi
      return 1
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
    tail -n 200 "$app_log_path" >&2 || true
    if [[ -f "$vg_log_path" ]]; then
      tail -n 200 "$vg_log_path" >&2 || true
    fi
    return 1
  fi
}

stop_host_app() {
  if [[ -n "${APP_PID:-}" ]] && kill -0 "$APP_PID" 2>/dev/null; then
    kill "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
  fi
  APP_PID=""
}

mkdir -p "$LOG_DIR"
rm -f "$APP_LOG" "$APP_LOG_SMOKE" "$APP_LOG_RGBWW" "$LOG_DIR/info.json" "$HTTP_TRACE_LOG" "$MALFORMED_JSON_TRACE" "$VALGRIND_LOG_SMOKE" "$VALGRIND_LOG_RGBWW"
{
    echo "host_ci_smoke_test started"
    echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "pwd=$(pwd)"
    echo "tap_if=$TAP_IF"
    echo "host_cidr=$HOST_CIDR"
    echo "app_ip=$APP_IP"
} > "$LOG_DIR/run-info.txt"

if ! ensure_ip_tool; then
    echo "cannot configure TAP interface without ip command" >&2
    exit 1
fi

ensure_valgrind

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

make SMING_ARCH=Host configdb-rebuild
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

ensure_pytest

export HOST_SMOKE_APP_IP="$APP_IP"
export HOST_SMOKE_BASE_URL="http://${APP_IP}"
export HOST_SMOKE_INFO_URL="$INFO_URL"
export HOST_SMOKE_COLOR_URL="$COLOR_URL"
export HOST_SMOKE_WS_HOST="$WS_HOST"
export HOST_SMOKE_WS_PORT="$WS_PORT"
export HOST_SMOKE_WS_PATH="$WS_PATH"
export HOST_SMOKE_LOG_DIR="$LOG_DIR"

TEST_REPORT="${LOG_DIR}/test-results.md"
TEST_OUTPUT="${LOG_DIR}/test-output.txt"
SMOKE_TEST_REPORT="${LOG_DIR}/smoke-test-results.md"
SMOKE_TEST_OUTPUT="${LOG_DIR}/smoke-test-output.txt"
RGBWW_TEST_REPORT="${LOG_DIR}/rgbww-test-results.md"
RGBWW_TEST_OUTPUT="${LOG_DIR}/rgbww-test-output.txt"

python3 -m pip install --quiet pytest-md

start_host_app "$APP_LOG_SMOKE" "$VALGRIND_LOG_SMOKE" "smoke"
python3 -m pytest \
  -v \
  --md "$SMOKE_TEST_REPORT" \
  tests/host_smoke_api_test.py 2>&1 | tee "$SMOKE_TEST_OUTPUT" || true
stop_host_app

start_host_app "$APP_LOG_RGBWW" "$VALGRIND_LOG_RGBWW" "rgbww"
python3 -m pytest \
  -v \
  --md "$RGBWW_TEST_REPORT" \
  tests/rgbww_test.py 2>&1 | tee "$RGBWW_TEST_OUTPUT" || true
stop_host_app

{
  echo "# Host CI Test Results"
  echo
  echo "## Smoke"
  cat "$SMOKE_TEST_REPORT"
  echo
  echo "## RGBWW"
  cat "$RGBWW_TEST_REPORT"
} > "$TEST_REPORT"

{
  echo "===== Smoke test output ====="
  cat "$SMOKE_TEST_OUTPUT"
  echo
  echo "===== RGBWW test output ====="
  cat "$RGBWW_TEST_OUTPUT"
} > "$TEST_OUTPUT"

{
  echo "===== Smoke app log ====="
  cat "$APP_LOG_SMOKE"
  echo
  echo "===== RGBWW app log ====="
  cat "$APP_LOG_RGBWW"
} > "$APP_LOG"

echo "Host smoke test completed"
tail -n 40 "$APP_LOG_RGBWW"