import json
import pytest
import requests
import time
from helpers import step, recv_notification


def test_color_hsv(base_url, ws_client):
    """POST /color HSV — LED updates and WS broadcast is received"""
    url = f"{base_url}/color"
    payload = {"hsv": {"h": 240, "s": 100, "v": 100}, "t": 100}

    resp = requests.post(url, json=payload)
    step("POST /color returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")

    notification = recv_notification(ws_client, "color_event")
    step("WS color_event notification received", notification is not None)
    if notification:
        h_val = float(notification.get("hsv", {}).get("h", -1))
        step("notification H ≈ 240°", 235 <= h_val <= 245, f"H={h_val}")

    http_state = requests.get(url).json()
    h_http = float(http_state.get("hsv", {}).get("h", -1))
    step("GET /color confirms H ≈ 240°", 235 <= h_http <= 245, f"H={h_http}")


def test_color_raw(base_url, ws_client):
    """POST /color raw — r/g/b values are reflected in GET /color response"""
    url = f"{base_url}/color"
    target = {"r": 512, "g": 0, "b": 1023, "ww": 0, "cw": 0}
    payload = {"raw": target, "t": 100}

    resp = requests.post(url, json=payload)
    step("POST /color raw returns HTTP 200", resp.status_code == 200, f"got {resp.status_code}")

    notification = recv_notification(ws_client, "color_event")
    step("WS color_event notification received", notification is not None)

    time.sleep(0.3)
    http_state = requests.get(url).json()
    step("GET /color response includes 'raw' object", "raw" in http_state)
    got_raw = http_state.get("raw", {})
    for ch in ("r", "g", "b", "ww", "cw"):
        step(f"raw.{ch} matches sent value", got_raw.get(ch) == target[ch],
             f"expected {target[ch]}, got {got_raw.get(ch)}")


def test_on_off_behavior(base_url):
    """POST /off then /on — brightness toggles to 0 and back"""
    requests.post(f"{base_url}/color", json={"hsv": {"h": 0, "s": 0, "v": 100}})
    time.sleep(0.5)

    resp_off = requests.post(f"{base_url}/off")
    step("POST /off returns HTTP 200", resp_off.status_code == 200, f"got {resp_off.status_code}")
    time.sleep(0.5)
    off_state = requests.get(f"{base_url}/color").json()
    v_off = float(off_state.get("hsv", {}).get("v", -1))
    step("brightness is 0 after OFF", v_off == 0, f"v={v_off}")

    resp_on = requests.post(f"{base_url}/on")
    step("POST /on returns HTTP 200", resp_on.status_code == 200, f"got {resp_on.status_code}")
    time.sleep(0.5)
    on_state = requests.get(f"{base_url}/color").json()
    v_on = float(on_state.get("hsv", {}).get("v", -1))
    step("brightness restored to 100 after ON", v_on == 100, f"v={v_on}")
