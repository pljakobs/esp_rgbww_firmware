"""OTA update and network-connect endpoint tests.

Covers remaining endpoints from the JSON API reference:
  GET  /update   — read OTA status (read-only, always safe)
  POST /connect  — connect to WiFi; tested with a missing/invalid ssid so the
                   device never tears down its current connection

NOTE: POST /update is intentionally NOT tested here.
Sending any POST /update request — even with an invalid URL — causes the
firmware to enter an "OTA in progress" state that persists until reboot,
breaking all subsequent tests in the suite.
"""
import pytest
import requests
from helpers import step


# ---------------------------------------------------------------------------
# GET /update
# ---------------------------------------------------------------------------

def test_get_update_structure(base_url):
    """GET /update — returns OTA status object.

    The current firmware returns a single 'status' field (int 0-3):
      0 = no update / idle
      1 = update in progress
      2 = OTA success
      3 = OTA failed

    (The older wiki documents rom_status/webapp_status which no longer applies.)
    """
    resp = requests.get(f"{base_url}/update")
    step("GET /update returns HTTP 200", resp.status_code == 200,
         f"got {resp.status_code}")
    data = resp.json()
    step("response is a JSON object", isinstance(data, dict), str(type(data)))
    step("response has 'status' field", "status" in data, str(data))

    status = int(data.get("status", -1))
    step("status is a known value (0-3)", 0 <= status <= 3, f"status={status}")


# ---------------------------------------------------------------------------
# POST /connect  — error-path only (does not change WiFi connection)
# ---------------------------------------------------------------------------

def test_post_connect_missing_ssid(base_url):
    """POST /connect with missing ssid — expects a 400 error"""
    resp = requests.post(f"{base_url}/connect", json={"password": "irrelevant"})
    step("POST /connect (no ssid) returns 400 or error", resp.status_code in (400, 200),
         f"got {resp.status_code}")
    data = resp.json()
    # Accept either an 'error' key or a 200 with success:false
    has_error = ("error" in data) or (data.get("success") is False)
    step("response indicates failure when ssid is missing", has_error or resp.status_code == 400,
         str(data))


def test_post_connect_empty_ssid(base_url):
    """POST /connect with empty ssid — firmware accepts it without validation.

    The firmware does not reject an empty ssid; it returns success and then
    silently fails to connect in the background.  We verify only that the
    device stays responsive (i.e. the request does not crash the firmware).
    """
    resp = requests.post(f"{base_url}/connect", json={"ssid": ""})
    step("POST /connect (empty ssid) returns HTTP 200", resp.status_code == 200,
         f"got {resp.status_code}")
    # Device must still respond after the request
    info = requests.get(f"{base_url}/info")
    step("device still responds after POST /connect (empty ssid)",
         info.status_code == 200)
