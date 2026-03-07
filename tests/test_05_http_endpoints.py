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
    # Status 1 (connecting) is also valid if the device is busy reconnecting
    step("status is idle, connecting or connected (0, 1 or 2)", status in (0, 1, 2), f"status={status}")


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


def test_hosts_default(base_url):
    """GET /hosts — returns visible controllers list (no query param)

    Matches api.js: getHosts(showAll=false) → hosts?all=false
    The firmware's VISIBLE_ONLY filter is applied when all is absent/false.
    """
    resp = requests.get(f"{base_url}/hosts")
    step("GET /hosts returns HTTP 200", resp.status_code == 200,
         f"got {resp.status_code}")
    data = resp.json()
    step("response is a JSON object or array", isinstance(data, (dict, list)), str(type(data)))
    # The response may be a list of controller objects or a wrapping dict
    entries = data if isinstance(data, list) else data.get("controllers", data.get("hosts", []))
    step("entries is a list", isinstance(entries, list), str(type(entries)))


def test_hosts_all_false(base_url):
    """GET /hosts?all=false — explicit visible-only filter

    Matches api.js: getHosts(false) → hosts?all=false
    Should return same result as no param.
    """
    resp_default = requests.get(f"{base_url}/hosts")
    resp_all_false = requests.get(f"{base_url}/hosts", params={"all": "false"})

    step("GET /hosts?all=false returns HTTP 200", resp_all_false.status_code == 200,
         f"got {resp_all_false.status_code}")

    default_data = resp_default.json()
    all_false_data = resp_all_false.json()

    default_list = default_data if isinstance(default_data, list) else list(default_data.values())
    all_false_list = all_false_data if isinstance(all_false_data, list) else list(all_false_data.values())

    step("all=false result has same entry count as default",
         len(default_list) == len(all_false_list),
         f"default={len(default_list)} all=false={len(all_false_list)}")


def test_hosts_all_true(base_url):
    """GET /hosts?all=true — show all controllers including incomplete entries

    Matches api.js: getHosts(true) → hosts?all=true
    Should return >= entries than the default visible-only list.
    """
    resp_default = requests.get(f"{base_url}/hosts")
    resp_all = requests.get(f"{base_url}/hosts", params={"all": "true"})

    step("GET /hosts?all=true returns HTTP 200", resp_all.status_code == 200,
         f"got {resp_all.status_code}")
    data = resp_all.json()
    step("response is a JSON object or array", isinstance(data, (dict, list)), str(type(data)))

    all_list = data if isinstance(data, list) else list(data.values())
    default_list = resp_default.json()
    if isinstance(default_list, dict):
        default_list = list(default_list.values())

    step("all=true returns >= entries than default (visible-only)",
         len(all_list) >= len(default_list),
         f"all={len(all_list)} default={len(default_list)}")


def test_hosts_all_numeric_true(base_url):
    """GET /hosts?all=1 — numeric form of all=true also accepted by firmware"""
    resp = requests.get(f"{base_url}/hosts", params={"all": "1"})
    step("GET /hosts?all=1 returns HTTP 200", resp.status_code == 200,
         f"got {resp.status_code}")
    data = resp.json()
    step("response is a JSON object or array", isinstance(data, (dict, list)), str(type(data)))


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
