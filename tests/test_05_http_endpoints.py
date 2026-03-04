"""HTTP API endpoint tests based on the JSON API reference.

Covers endpoints not tested elsewhere:
  GET /ping
  GET /networks
  POST /scan_networks
  GET /color?mode=hsv
  GET /color?mode=raw
  POST /system cmd:debug  (safe — just enables/disables debug output)
  GET /connect            (connection status, read-only)
"""
import pytest
import requests
import time
from helpers import step


def test_ping(base_url):
    """GET /ping — must return {'ping': 'pong'}"""
    resp = requests.get(f"{base_url}/ping")
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("body has 'ping' key", "ping" in data)
    step("ping value is 'pong'", data.get("ping") == "pong", repr(data.get("ping")))


def test_networks_structure(base_url):
    """GET /networks — must return scanning flag and available list"""
    resp = requests.get(f"{base_url}/networks")
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("body has 'available' key", "available" in data)
    step("'available' is a list", isinstance(data.get("available"), list))
    step("body has 'scanning' key", "scanning" in data)


def test_scan_networks(base_url):
    """POST /scan_networks — initiates a scan, returns success"""
    resp = requests.post(f"{base_url}/scan_networks")
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("response indicates success", data.get("success") is True or resp.status_code == 200,
         str(data))


def test_connect_status(base_url):
    """GET /connect — returns current connection state machine status (0-3).

    0=idle (not attempting), 1=connecting, 2=success, 3=failed.
    When already connected, the device returns 0 (idle, no active attempt).
    """
    resp = requests.get(f"{base_url}/connect")
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("response has 'status' field", "status" in data)
    status = int(data.get("status", -1))
    step("status is a known value (0-3)", 0 <= status <= 3, f"status={status}")
    # After a successful connection the firmware resets status to 0 (idle)
    step("status is idle or connected (0 or 2)", status in (0, 2), f"status={status}")


def test_color_mode_hsv(base_url):
    """GET /color?mode=hsv — returns HSV values"""
    resp = requests.get(f"{base_url}/color", params={"mode": "hsv"})
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("response has 'hsv' key", "hsv" in data)
    hsv = data.get("hsv", {})
    for field in ("h", "s", "v"):
        step(f"hsv.{field} is present", field in hsv)
    h = float(hsv.get("h", -1))
    s = float(hsv.get("s", -1))
    v = float(hsv.get("v", -1))
    step("h in [0, 360]", 0 <= h <= 360, f"h={h}")
    step("s in [0, 100]", 0 <= s <= 100, f"s={s}")
    step("v in [0, 100]", 0 <= v <= 100, f"v={v}")


def test_color_mode_raw(base_url):
    """GET /color?mode=raw — returns raw channel values"""
    resp = requests.get(f"{base_url}/color", params={"mode": "raw"})
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("response has 'raw' key", "raw" in data)
    raw = data.get("raw", {})
    for ch in ("r", "g", "b", "ww", "cw"):
        step(f"raw.{ch} is present", ch in raw)
        val = int(raw.get(ch, -1))
        step(f"raw.{ch} in [0, 1023]", 0 <= val <= 1023, f"{ch}={val}")


def test_color_default_response(base_url):
    """GET /color (no mode param) — returns both raw and hsv"""
    resp = requests.get(f"{base_url}/color")
    step("returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")
    data = resp.json()
    step("response has 'hsv' key", "hsv" in data)
    step("response has 'raw' key", "raw" in data)


def test_system_debug_toggle(base_url):
    """POST /system cmd:debug — toggles debug output (safe, no side effects)"""
    # Enable debug
    resp = requests.post(f"{base_url}/system", json={"cmd": "debug", "enable": "true"})
    step("POST /system debug=true returns HTTP 200", resp.status_code == 200,
         f"got {resp.status_code}")
    data = resp.json()
    step("response indicates success", data.get("success") is True, str(data))

    # Disable debug
    resp2 = requests.post(f"{base_url}/system", json={"cmd": "debug", "enable": "false"})
    step("POST /system debug=false returns HTTP 200", resp2.status_code == 200,
         f"got {resp2.status_code}")
    data2 = resp2.json()
    step("response indicates success", data2.get("success") is True, str(data2))
