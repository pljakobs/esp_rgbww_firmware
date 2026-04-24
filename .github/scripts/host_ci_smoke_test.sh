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
HTTP_TRACE_LOG="${LOG_DIR}/http-probes.jsonl"
MALFORMED_JSON_TRACE="${LOG_DIR}/malformed-color-response.json"
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

mkdir -p "$LOG_DIR"
rm -f "$APP_LOG" "$LOG_DIR/info.json" "$HTTP_TRACE_LOG" "$MALFORMED_JSON_TRACE"

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

set +u
source "$export_script"
set -u

make SMING_ARCH=Host configdb-rebuild
make SMING_ARCH=Host flash DISABLE_WERROR=1 COM_SPEED=115200

FLASH_BIN="${FIRMWARE_DIR}/flash.bin"
PARTITIONS_BIN="${FIRMWARE_DIR}/partitions.bin"
HOST_CONFIG_MK="out/Host/debug/config.mk"

if [[ ! -f "$PARTITIONS_BIN" ]]; then

    def send_text(self, text: str):
        payload = text.encode("utf-8")
        fin_opcode = 0x81
        length = len(payload)
        mask_bit = 0x80

        header = bytearray([fin_opcode])
        if length < 126:
            header.append(mask_bit | length)
        elif length <= 0xFFFF:
            header.append(mask_bit | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(mask_bit | 127)
            header.extend(struct.pack("!Q", length))

        mask = struct.pack("!I", random.getrandbits(32))
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + mask + masked)

    def recv_text(self, timeout: float = 5.0) -> str:
        self.sock.settimeout(timeout)
        b1, b2 = self._recv_exact(2)
        opcode = b1 & 0x0F
        masked = (b2 & 0x80) != 0
        length = b2 & 0x7F

        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]

        mask = self._recv_exact(4) if masked else b""
        payload = self._recv_exact(length) if length else b""
        if masked:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))

        if opcode == 0x8:
            fail("WebSocket close frame received unexpectedly")
        if opcode != 0x1:
            fail(f"Unsupported websocket opcode: {opcode}")

        return payload.decode("utf-8", errors="replace")

    def recv_json(self, timeout: float = 5.0):
        return json.loads(self.recv_text(timeout=timeout))

    def request(self, method: str, params=None, req_id: int = 1, timeout: float = 5.0):
        if params is None:
            params = {}
        self.send_text(json.dumps({
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method,
            "params": params,
        }))
        started = time.time()
        while time.time() - started < timeout:
            msg = self.recv_json(timeout=max(0.1, timeout - (time.time() - started)))
            if msg.get("id") == req_id:
                return msg
        fail(f"Timed out waiting for WS response to method {method}")

    def wait_for_method(self, method_name: str, timeout: float = 8.0):
        started = time.time()
        while time.time() - started < timeout:
            msg = self.recv_json(timeout=max(0.1, timeout - (time.time() - started)))
            if msg.get("method") == method_name:
                return msg
        fail(f"Timed out waiting for WS method event: {method_name}")


# Baseline checks
status, ping_payload = http_json("/ping")
if status != 200 or ping_payload.get("ping") != "pong":
    fail(f"Unexpected /ping response: status={status}, payload={ping_payload}")

status, info_payload = http_json("/info")
required_keys = ["git_version", "build_type", "sming", "connection"]
missing_keys = [key for key in required_keys if key not in info_payload]
if missing_keys:
    fail(f"Missing keys in /info response: {', '.join(missing_keys)}")

actual_ip = info_payload.get("connection", {}).get("ip")
if actual_ip != app_ip:
    fail(f"Unexpected Host IP in /info: {actual_ip!r} != {app_ip!r}")

with open(info_output_path, "w", encoding="utf-8") as handle:
    json.dump(info_payload, handle, indent=2, sort_keys=True)

print(json.dumps({
    "git_version": info_payload.get("git_version"),
    "build_type": info_payload.get("build_type"),
    "sming": info_payload.get("sming"),
    "ip": actual_ip,
}, sort_keys=True))

# Additional endpoint coverage
for endpoint in ["/color", "/networks", "/hosts", "/config", "/connect", "/data"]:
    status, payload = http_json(endpoint)
    if status != 200 or not isinstance(payload, (dict, list)):
        fail(f"Endpoint check failed for GET {endpoint}: status={status}, payload={payload}")

status, update_payload = http_json_expect_error("/update", "POST", "{}")
if status != 400 or "error" not in update_payload:
    fail(f"Expected Host /update POST to fail with API error, got: status={status}, payload={update_payload}")

for endpoint, body in [
    ("/scan_networks", {}),
    ("/system", {"cmd": "debug", "enable": False}),
    ("/stop", {}),
    ("/skip", {}),
    ("/pause", {}),
    ("/continue", {}),
    ("/blink", {}),
    ("/toggle", {}),
]:
    status, payload = http_json(endpoint, method="POST", payload=body)
    if status != 200 or payload.get("success") is not True:
        fail(f"Endpoint check failed for POST {endpoint}: status={status}, payload={payload}")

# Malformed JSON checks (HTTP)
status, malformed_payload = http_json_expect_error("/color", "POST", "{bad json")
if status != 400 or malformed_payload.get("error") != "Invalid JSON":
    fail(f"Expected Invalid JSON error for /color, got: status={status}, payload={malformed_payload}")

status, malformed_payload = http_json_expect_error("/on", "POST", "{bad json")
if status != 400 or malformed_payload.get("error") != "Invalid JSON":
    fail(f"Expected Invalid JSON error for /on, got: status={status}, payload={malformed_payload}")

# WS interaction + cross-interface transparency + event updates
ws = WebSocketClient(ws_host, ws_port, ws_path, timeout=5.0)

# WS malformed request checks
ws.send_text("{bad json")
msg = ws.recv_json(timeout=3.0)
if "malformed json" not in str(msg.get("error", "")):
    fail(f"Expected WS malformed-json rejection, got: {msg}")

ws.send_text(json.dumps({"jsonrpc": "2.0", "id": 7001, "params": {}}))
msg = ws.recv_json(timeout=3.0)
if msg.get("id") != 7001 or "missing method" not in str(msg.get("error", "")):
    fail(f"Expected WS missing-method rejection, got: {msg}")

status, http_color_before = http_json("/color")
if status != 200 or "raw" not in http_color_before:
    fail(f"Failed to read baseline color: status={status}, payload={http_color_before}")

# Setter test via HTTP, read back via WS and HTTP
status, set_resp = http_json("/color", method="POST", payload={"raw": {"r": 12, "g": 34, "b": 56, "ww": 78, "cw": 90}})
if status != 200 or set_resp.get("success") is not True:
    fail(f"HTTP /color setter failed: status={status}, payload={set_resp}")

changed = False
http_after_set = None
for _ in range(20):
    status, http_after_set = http_json("/color")
    if status == 200 and http_after_set.get("raw") != http_color_before.get("raw"):
        changed = True
        break
    time.sleep(0.1)
if not changed:
    fail("HTTP /color setter did not change color state")

ws_get = ws.request("getColor", req_id=8001)
ws_color_after_http_set = ws_get.get("result", {})
if ws_color_after_http_set.get("raw") != http_after_set.get("raw"):
    fail(
        "Transparency check failed: HTTP-set color differs between HTTP and WS reads "
        f"(http={http_after_set.get('raw')}, ws={ws_color_after_http_set.get('raw')})"
    )

# Setter test via WS, read back via HTTP and WS
prev_raw = http_after_set.get("raw")
ws_set_resp = ws.request("color", params={"raw": {"r": 90, "g": 10, "b": 20, "ww": 30, "cw": 40}}, req_id=8002)
if ws_set_resp.get("result", {}).get("success") is not True:
    fail(f"WS color setter failed: {ws_set_resp}")

changed = False
http_after_ws_set = None
for _ in range(20):
    status, http_after_ws_set = http_json("/color")
    if status == 200 and http_after_ws_set.get("raw") != prev_raw:
        changed = True
        break
    time.sleep(0.1)
if not changed:
    fail("WS /color setter did not change HTTP-observed color state")

ws_get_after_ws_set = ws.request("getColor", req_id=8003).get("result", {})
if ws_get_after_ws_set.get("raw") != http_after_ws_set.get("raw"):
    fail(
        "Transparency check failed: WS-set color differs between HTTP and WS reads "
        f"(http={http_after_ws_set.get('raw')}, ws={ws_get_after_ws_set.get('raw')})"
    )

# WS status updates while testing /off and /on
status, off_resp = http_json("/off", method="POST", payload={})
if status != 200 or off_resp.get("success") is not True:
    fail(f"/off failed: status={status}, payload={off_resp}")

off_event = ws.wait_for_method("color_event", timeout=8.0)
off_raw = off_event.get("params", {}).get("raw", {})
if any(int(off_raw.get(ch, 0)) != 0 for ch in ("r", "g", "b", "ww", "cw")):
    fail(f"Expected /off color_event with zeroed raw channels, got: {off_event}")

status, on_resp = http_json("/on", method="POST", payload={})
if status != 200 or on_resp.get("success") is not True:
    fail(f"/on failed: status={status}, payload={on_resp}")

on_event = ws.wait_for_method("color_event", timeout=8.0)
on_raw = on_event.get("params", {}).get("raw", {})
if all(int(on_raw.get(ch, 0)) == 0 for ch in ("r", "g", "b", "ww", "cw")):
    fail(f"Expected /on color_event with at least one non-zero channel, got: {on_event}")

print("Extended HTTP/WS endpoint tests passed")

# Test mDNS hosts endpoint
hosts_status, hosts_resp = http_json("/hosts")
if hosts_status != 200:
    fail(f"/hosts endpoint failed: status={hosts_status}")

if not isinstance(hosts_resp, dict):
    fail(f"Expected /hosts to return JSON object, got: {type(hosts_resp)}")

print(f"mDNS hosts endpoint test passed: {json.dumps(hosts_resp)}")

# Test webapp static resources
webapp_tests = [
    ("/", "root index"),
    ("/webapp", "webapp interface"),
]

for path, desc in webapp_tests:
    try:
        req = urllib.request.Request(base_url + path, method="GET")
        with urllib.request.urlopen(req, timeout=5.0) as response:
            content = response.read().decode("utf-8", errors="replace")
            if response.status != 200:
                fail(f"Webapp {desc} returned status {response.status}")
            
            # Check for basic HTML structure
            if not any(marker in content.lower() for marker in ["<!doctype", "<html", "<!DOCTYPE", "<HTML"]):
                if len(content) < 100 and "error" in content.lower():
                    fail(f"Webapp {desc} returned error: {content[:200]}")
                # Some pages might not be full HTML, just check they're not empty
                if len(content) == 0:
                    fail(f"Webapp {desc} returned empty response")
            
            print(f"Webapp {desc} loaded successfully ({len(content)} bytes)")
    except urllib.error.HTTPError as e:
        fail(f"Webapp {desc} failed with HTTP {e.code}: {e.reason}")
    except Exception as e:
        fail(f"Webapp {desc} failed: {type(e).__name__}: {e}")

# Test other critical endpoints for coverage
critical_endpoints = [
    ("/data", "GET", "system data"),
    ("/connect", "GET", "connection info"),
]

for path, method, desc in critical_endpoints:
    try:
        status, resp = http_json(path, method=method)
        if status != 200:
            fail(f"Critical endpoint {desc} ({path}) returned {status}")
        if not isinstance(resp, (dict, list)):
            fail(f"Critical endpoint {desc} returned invalid type: {type(resp)}")
        print(f"Critical endpoint {desc} test passed")
    except Exception as e:
        fail(f"Critical endpoint {desc} failed: {e}")

print("Webapp and mDNS tests passed")
PY
>>>>>>> 591ea99 (chore: Host emulator tests and CI improvements)

# Write test summary report for GitHub Actions
{
    echo "## Host Emulator Smoke Test Results"
    echo ""
    echo "✅ **All tests passed**"
    echo ""
    echo "### Test Coverage"
    echo "- ✅ Basic connectivity (/ping endpoint)"
    echo "- ✅ WebSocket RFC 6455 handshake and JSON-RPC 2.0 protocol"
    echo "- ✅ HTTP API endpoints (15+ endpoints)"
    echo "- ✅ Setter functionality with round-trip verification"
    echo "- ✅ Cross-interface transparency (HTTP↔WS consistency)"
    echo "- ✅ Malformed JSON error handling (HTTP + WS)"
    echo "- ✅ Event subscription (color_event notifications)"
    echo "- ✅ mDNS hosts endpoint"
    echo "- ✅ Webapp static resources"
    echo "- ✅ Critical system endpoints"
    echo ""
    echo "### Environment"
    echo "- Host: $(uname -s) $(uname -m)"
    echo "- Sming: $(grep -oP 'Sming Version: \K.*' "$APP_LOG" | head -1)"
    echo "- Firmware: $(grep -oP 'Version \K[^ ]*' "$APP_LOG" | head -1)"
    echo ""
    echo "### Log"
    echo "\`\`\`"
    tail -n 30 "$APP_LOG"
    echo "\`\`\`"
} > "${LOG_DIR}/test_summary.md"

# Output GitHub Actions annotations for workflow visibility
if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    echo "::notice title=Host Emulator Smoke Test::All tests passed successfully"
    echo "::group::Test Summary"
    cat "${LOG_DIR}/test_summary.md"
    echo "::endgroup::"
fi

echo ""
echo "Host smoke test passed"
echo "Test summary: ${LOG_DIR}/test_summary.md"
echo "Full log: ${APP_LOG}"
tail -n 40 "$APP_LOG"