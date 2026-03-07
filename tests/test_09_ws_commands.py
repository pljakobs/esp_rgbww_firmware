"""WebSocket JSON-RPC tests for all control/query endpoints.

Covers the WS equivalents of HTTP endpoints not already exercised by
test_02_consistency.py and test_04_ws_control.py.

Already covered elsewhere:
  color (HSV/RAW command, notification, query)  →  test_04_ws_control.py
  info / config / data / color consistency      →  test_02_consistency.py

New coverage here:
  on        — Command
  off       — Command
  toggle    — Command (two-call cycle)
  stop      — Command (no channels / specific channels)
  pause     — Command
  continue  — Command
  skip      — Command
  blink     — Command
  networks  — Query (empty params → list) and Command (non-empty params → scan)
  system    — Command {"cmd":"debug"} (only safe system command)

Firmware auto-detection notes:
  • on/off/stop/pause/continue/skip/blink/toggle are NOT in the Query
    auto-detect whitelist → always treated as Command regardless of params.
  • networks IS in the whitelist → empty params ⇒ Query; non-empty ⇒ Command.
  • system IS in the whitelist → empty params ⇒ Query → 405; must always
    be called with {"cmd": ...} params.
"""
import json
import pytest
import requests
import time
from helpers import step, recv_ws_id, recv_notification


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _ws_call(ws_client, req_id: int, method: str, params: dict):
    """Send a JSON-RPC request and wait for the matching response."""
    ws_client.send(json.dumps({
        "jsonrpc": "2.0",
        "method": method,
        "params": params,
        "id": req_id,
    }))
    resp = recv_ws_id(ws_client, req_id)
    time.sleep(0.5)
    return resp


def _set_known_color(base_url, h=90, s=100, v=100, t=50):
    requests.post(f"{base_url}/color", json={"hsv": {"h": h, "s": s, "v": v}, "t": t})
    time.sleep(0.3)


def _assert_ok(response, label=""):
    prefix = f"{label}: " if label else ""
    step(f"{prefix}WS response received", response is not None)
    step(f"{prefix}WS response has no error",
         response is not None and "error" not in response,
         str(response.get("error") if response else "no response"))


# ---------------------------------------------------------------------------
# on / off
# ---------------------------------------------------------------------------

def test_ws_off(ws_client, base_url):
    """WS cmd:off — brightness drops to 0, confirmed via GET /color"""
    _set_known_color(base_url, v=100)

    resp = _ws_call(ws_client, 20, "off", {})
    _assert_ok(resp, "WS off")

    time.sleep(0.4)
    v = float(requests.get(f"{base_url}/color").json().get("hsv", {}).get("v", -1))
    step("GET /color confirms v=0 after WS off", v == 0.0, f"v={v}")


def test_ws_on(ws_client, base_url):
    """WS cmd:on — brightness restored, confirmed via GET /color"""
    # Ensure we're off first
    requests.post(f"{base_url}/off")
    time.sleep(0.3)

    resp = _ws_call(ws_client, 21, "on", {})
    _assert_ok(resp, "WS on")

    time.sleep(0.4)
    v = float(requests.get(f"{base_url}/color").json().get("hsv", {}).get("v", -1))
    step("GET /color confirms v>0 after WS on", v > 0, f"v={v}")


def test_ws_on_off_sequence(ws_client, base_url):
    """WS cmd:off then cmd:on — full round-trip via WS only"""
    _set_known_color(base_url, h=60, s=100, v=100)

    resp_off = _ws_call(ws_client, 22, "off", {})
    _assert_ok(resp_off, "WS off")
    time.sleep(0.4)

    v_off = float(requests.get(f"{base_url}/color").json().get("hsv", {}).get("v", -1))
    step("v=0 after WS off", v_off == 0.0, f"v={v_off}")

    resp_on = _ws_call(ws_client, 23, "on", {})
    _assert_ok(resp_on, "WS on")
    time.sleep(0.4)

    v_on = float(requests.get(f"{base_url}/color").json().get("hsv", {}).get("v", -1))
    step("v>0 after WS on", v_on > 0, f"v={v_on}")


# ---------------------------------------------------------------------------
# toggle
# ---------------------------------------------------------------------------

def test_ws_toggle_cycle(ws_client, base_url):
    """WS cmd:toggle — two calls cycle brightness off then on"""
    _set_known_color(base_url, h=90, s=100, v=100)

    resp1 = _ws_call(ws_client, 24, "toggle", {})
    _assert_ok(resp1, "WS toggle (1st)")
    time.sleep(0.4)

    v_off = float(requests.get(f"{base_url}/color").json().get("hsv", {}).get("v", -1))
    step("v=0 after first WS toggle", v_off == 0.0, f"v={v_off}")

    resp2 = _ws_call(ws_client, 25, "toggle", {})
    _assert_ok(resp2, "WS toggle (2nd)")
    time.sleep(0.4)

    v_on = float(requests.get(f"{base_url}/color").json().get("hsv", {}).get("v", -1))
    step("v>0 after second WS toggle", v_on > 0, f"v={v_on}")


# ---------------------------------------------------------------------------
# stop
# ---------------------------------------------------------------------------

def test_ws_stop(ws_client, base_url):
    """WS cmd:stop — clears queue, device remains responsive"""
    _set_known_color(base_url)

    resp = _ws_call(ws_client, 30, "stop", {})
    _assert_ok(resp, "WS stop")

    info = requests.get(f"{base_url}/info")
    step("device responds to GET /info after WS stop", info.status_code == 200)


def test_ws_stop_specific_channels(ws_client, base_url):
    """WS cmd:stop with channels — stops only specified channels"""
    _set_known_color(base_url)

    resp = _ws_call(ws_client, 31, "stop", {"channels": ["h", "s", "v"]})
    _assert_ok(resp, "WS stop channels")


# ---------------------------------------------------------------------------
# pause / continue
# ---------------------------------------------------------------------------

def test_ws_pause_and_continue(ws_client, base_url):
    """WS cmd:pause then cmd:continue — both succeed, device stays alive"""
    # Start a slow fade
    requests.post(f"{base_url}/color",
                  json={"hsv": {"h": 30, "s": 100, "v": 100}, "t": 3000})
    time.sleep(0.1)

    resp_pause = _ws_call(ws_client, 32, "pause", {})
    _assert_ok(resp_pause, "WS pause")

    time.sleep(0.2)

    resp_cont = _ws_call(ws_client, 33, "continue", {})
    _assert_ok(resp_cont, "WS continue")

    requests.post(f"{base_url}/stop", json={})


def test_ws_pause_specific_channels(ws_client, base_url):
    """WS cmd:pause with specific channels"""
    requests.post(f"{base_url}/color",
                  json={"hsv": {"h": 200, "s": 100, "v": 100}, "t": 2000})
    time.sleep(0.1)

    resp = _ws_call(ws_client, 34, "pause", {"channels": ["h"]})
    _assert_ok(resp, "WS pause channels")

    requests.post(f"{base_url}/stop", json={})


# ---------------------------------------------------------------------------
# skip
# ---------------------------------------------------------------------------

def test_ws_skip(ws_client, base_url):
    """WS cmd:skip — skips animation, device stays responsive"""
    _set_known_color(base_url)
    requests.post(f"{base_url}/color",
                  json={"hsv": {"h": 270, "s": 100, "v": 100}, "t": 5000, "q": "back"})
    time.sleep(0.1)

    resp = _ws_call(ws_client, 35, "skip", {})
    _assert_ok(resp, "WS skip")

    time.sleep(0.2)
    step("device responds to GET /info after WS skip",
         requests.get(f"{base_url}/info").status_code == 200)

    requests.post(f"{base_url}/stop", json={})


# ---------------------------------------------------------------------------
# blink
# ---------------------------------------------------------------------------

def test_ws_blink(ws_client, base_url):
    """WS cmd:blink — succeeds and triggers a color_event broadcast"""
    _set_known_color(base_url, h=0, s=100, v=100)

    ws_client.send(json.dumps({
        "jsonrpc": "2.0",
        "method": "blink",
        "params": {"t": 300, "q": "single"},
        "id": 36,
    }))

    resp = recv_ws_id(ws_client, 36)
    _assert_ok(resp, "WS blink")

    notification = recv_notification(ws_client, "color_event", timeout_msgs=60)
    step("WS color_event broadcast received during blink", notification is not None)

    time.sleep(0.8)
    requests.post(f"{base_url}/stop", json={})


# ---------------------------------------------------------------------------
# networks — Query (list) and Command (scan)
# ---------------------------------------------------------------------------

def test_ws_networks_query(ws_client, base_url):
    """WS method:networks (empty params) — auto-detected as Query, returns list"""
    resp = _ws_call(ws_client, 40, "networks", {})
    step("WS networks query response received", resp is not None)
    step("WS networks has no error",
         resp is not None and "error" not in resp,
         str(resp.get("error") if resp else "no response"))

    if resp:
        result = resp.get("result", {})
        step("result has 'scanning' field", "scanning" in result, str(result))
        step("result has 'available' list", "available" in result, str(result))
        step("'available' is a list", isinstance(result.get("available"), list))


def test_ws_networks_scan(ws_client, base_url):
    """WS method:networks (non-empty params) — auto-detected as Command, initiates scan"""
    # Any non-empty params triggers Command path → scan
    resp = _ws_call(ws_client, 41, "networks", {"scan": True})
    step("WS networks scan response received", resp is not None)
    step("WS networks scan has no error",
         resp is not None and "error" not in resp,
         str(resp.get("error") if resp else "no response"))

    # After initiating a scan, GET /networks should show scanning=true (briefly)
    # or the scan may have already completed; either is valid
    time.sleep(1) # wait for scan to finish/update
    nw = requests.get(f"{base_url}/networks").json()
    step("GET /networks responds after WS scan trigger", "available" in nw)


def test_ws_networks_matches_http(ws_client, base_url):
    """WS networks query and GET /networks return consistent data"""
    http_nw = requests.get(f"{base_url}/networks").json()
    step("GET /networks has 'available' key", "available" in http_nw)

    resp = _ws_call(ws_client, 42, "networks", {})
    step("WS networks response received", resp is not None)
    if resp:
        ws_nw = resp.get("result", {})
        step("WS 'available' list has same length as HTTP",
             len(ws_nw.get("available", [])) == len(http_nw.get("available", [])),
             f"WS={len(ws_nw.get('available', []))} HTTP={len(http_nw.get('available', []))}")


# ---------------------------------------------------------------------------
# system — Command (debug toggle, safe)
# ---------------------------------------------------------------------------

def test_ws_system_debug_enable(ws_client, base_url):
    """WS cmd:system {"cmd":"debug","enable":true} — succeeds"""
    resp = _ws_call(ws_client, 50, "system", {"cmd": "debug", "enable": True})
    _assert_ok(resp, "WS system debug enable")


def test_ws_system_debug_disable(ws_client, base_url):
    """WS cmd:system {"cmd":"debug","enable":false} — succeeds"""
    resp = _ws_call(ws_client, 51, "system", {"cmd": "debug", "enable": False})
    _assert_ok(resp, "WS system debug disable")


def test_ws_system_empty_params_returns_error(ws_client, base_url):
    """WS method:system with empty params — auto-detected as Query → 405 error"""
    # 'system' is in the auto-detect whitelist, so empty params → Query → 405
    resp = _ws_call(ws_client, 52, "system", {})
    step("WS system (empty params) returns a response", resp is not None)
    if resp:
        step("WS system (empty params) returns an error (405→-32601)",
             "error" in resp,
             str(resp))
