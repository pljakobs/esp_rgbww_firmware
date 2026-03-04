"""Animation control endpoint tests.

Covers the remaining animation-control endpoints from the JSON API reference:
  POST /stop
  POST /pause
  POST /continue
  POST /skip
  POST /blink
  POST /toggle
  GET  /on    (verify GET method is accepted in addition to POST)
  GET  /off
"""
import pytest
import requests
import time
from helpers import step, recv_notification


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _set_known_color(base_url, h=90, s=100, v=100, t=50):
    """Put the device in a known, fully-on HSV state before each test."""
    requests.post(f"{base_url}/color", json={"hsv": {"h": h, "s": s, "v": v}, "t": t})
    time.sleep(0.3)


# ---------------------------------------------------------------------------
# POST /stop
# ---------------------------------------------------------------------------

def test_stop_no_channels(base_url):
    """POST /stop (no channels) — clears queue and device stays responsive"""
    _set_known_color(base_url)

    resp = requests.post(f"{base_url}/stop", json={})
    step("POST /stop returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("response indicates success", data.get("success") is True, str(data))

    # Device must still respond after stop
    info = requests.get(f"{base_url}/info")
    step("device still responds after /stop", info.status_code == 200)


def test_stop_specific_channels(base_url):
    """POST /stop with channels=[h,s,v] — stop only HSV channels"""
    _set_known_color(base_url)

    resp = requests.post(f"{base_url}/stop", json={"channels": ["h", "s", "v"]})
    step("POST /stop (channels) returns HTTP 200", resp.status_code == 200,
         f"got {resp.status_code}")
    data = resp.json()
    step("response indicates success", data.get("success") is True, str(data))


# ---------------------------------------------------------------------------
# POST /pause  +  POST /continue
# ---------------------------------------------------------------------------

def test_pause_then_continue(base_url):
    """POST /pause then POST /continue — both return 200, device remains alive"""
    # Start a slow fade that we can then pause
    requests.post(f"{base_url}/color",
                  json={"hsv": {"h": 30, "s": 100, "v": 100}, "t": 3000})
    time.sleep(0.1)

    resp_pause = requests.post(f"{base_url}/pause", json={})
    step("POST /pause returns HTTP 200", resp_pause.status_code == 200,
         f"got {resp_pause.status_code}")
    data_pause = resp_pause.json()
    step("pause response indicates success", data_pause.get("success") is True, str(data_pause))

    time.sleep(0.2)

    resp_cont = requests.post(f"{base_url}/continue", json={})
    step("POST /continue returns HTTP 200", resp_cont.status_code == 200,
         f"got {resp_cont.status_code}")
    data_cont = resp_cont.json()
    step("continue response indicates success", data_cont.get("success") is True, str(data_cont))

    # Clean up – stop any remaining animation
    requests.post(f"{base_url}/stop", json={})


def test_pause_with_channels(base_url):
    """POST /pause with specific channels"""
    _set_known_color(base_url)
    requests.post(f"{base_url}/color",
                  json={"hsv": {"h": 200, "s": 100, "v": 100}, "t": 2000})
    time.sleep(0.1)

    resp = requests.post(f"{base_url}/pause", json={"channels": ["h"]})
    step("POST /pause (channels) returns HTTP 200", resp.status_code == 200,
         f"got {resp.status_code}")
    step("response indicates success", resp.json().get("success") is True, str(resp.json()))

    requests.post(f"{base_url}/stop", json={})


# ---------------------------------------------------------------------------
# POST /skip
# ---------------------------------------------------------------------------

def test_skip(base_url):
    """POST /skip — skips current animation and device stays responsive"""
    _set_known_color(base_url)

    # Queue up a slow animation then skip it
    requests.post(f"{base_url}/color",
                  json={"hsv": {"h": 270, "s": 100, "v": 100}, "t": 5000, "q": "back"})
    time.sleep(0.1)

    resp = requests.post(f"{base_url}/skip", json={})
    step("POST /skip returns HTTP 200", resp.status_code == 200,
         f"got {resp.status_code}")
    data = resp.json()
    step("response indicates success", data.get("success") is True, str(data))

    time.sleep(0.2)
    info = requests.get(f"{base_url}/info")
    step("device still responds after /skip", info.status_code == 200)

    requests.post(f"{base_url}/stop", json={})


# ---------------------------------------------------------------------------
# POST /blink
# ---------------------------------------------------------------------------

def test_blink(base_url, ws_client):
    """POST /blink — returns 200 and triggers a WS color_event notification"""
    _set_known_color(base_url, h=0, s=100, v=100)

    resp = requests.post(f"{base_url}/blink",
                         json={"t": 300, "q": "single"})
    step("POST /blink returns HTTP 200", resp.status_code == 200,
         f"got {resp.status_code}")
    data = resp.json()
    step("response indicates success", data.get("success") is True, str(data))

    # Should receive at least one color_event during the blink
    notification = recv_notification(ws_client, "color_event", timeout_msgs=60)
    step("WS color_event received during blink", notification is not None)

    # Wait for blink to finish then stop cleanly
    time.sleep(0.8)
    requests.post(f"{base_url}/stop", json={})


# ---------------------------------------------------------------------------
# POST /toggle
# ---------------------------------------------------------------------------

def test_toggle_off_and_on(base_url):
    """POST /toggle — first call turns off (v→0), second call restores brightness"""
    _set_known_color(base_url, h=60, s=100, v=100)
    time.sleep(0.2)

    # First toggle: should turn off
    resp1 = requests.post(f"{base_url}/toggle", json={})
    step("POST /toggle (1st) returns HTTP 200", resp1.status_code == 200,
         f"got {resp1.status_code}")
    step("1st toggle response indicates success", resp1.json().get("success") is True, str(resp1.json()))

    time.sleep(1.0)
    state_off = requests.get(f"{base_url}/color").json()
    v_off = float(state_off.get("hsv", {}).get("v", -1))
    step("brightness is 0 after first toggle", v_off == 0.0, f"v={v_off}")

    # Second toggle: should restore
    resp2 = requests.post(f"{base_url}/toggle", json={})
    step("POST /toggle (2nd) returns HTTP 200", resp2.status_code == 200,
         f"got {resp2.status_code}")
    step("2nd toggle response indicates success", resp2.json().get("success") is True, str(resp2.json()))

    time.sleep(1.0)
    state_on = requests.get(f"{base_url}/color").json()
    v_on = float(state_on.get("hsv", {}).get("v", -1))
    step("brightness is restored after second toggle", v_on > 0, f"v={v_on}")


# ---------------------------------------------------------------------------
# GET /on  /  GET /off  (GET method variants)
# ---------------------------------------------------------------------------

def test_get_on(base_url):
    """GET /on — same semantics as POST /on (turns device on)"""
    # Ensure we're off first
    requests.post(f"{base_url}/off")
    time.sleep(0.3)

    resp = requests.get(f"{base_url}/on")
    step("GET /on returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    step("response indicates success", resp.json().get("success") is True, str(resp.json()))

    time.sleep(0.3)
    state = requests.get(f"{base_url}/color").json()
    v = float(state.get("hsv", {}).get("v", -1))
    step("brightness > 0 after GET /on", v > 0, f"v={v}")


def test_get_off(base_url):
    """GET /off — same semantics as POST /off (turns device off)"""
    _set_known_color(base_url, v=100)
    time.sleep(0.2)

    resp = requests.get(f"{base_url}/off")
    step("GET /off returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    step("response indicates success", resp.json().get("success") is True, str(resp.json()))

    time.sleep(0.3)
    state = requests.get(f"{base_url}/color").json()
    v = float(state.get("hsv", {}).get("v", -1))
    step("brightness is 0 after GET /off", v == 0.0, f"v={v}")

    # Restore
    requests.post(f"{base_url}/on")
