"""WebSocket control tests — set color via JSON-RPC and verify via HTTP GET."""

import json
import pytest
import requests
import time
from helpers import step, recv_ws_id, recv_notification


def test_ws_color_set_hsv(ws_client, base_url):
    """WS cmd:color HSV — LED updates and HTTP confirms"""
    target_h = 180  # cyan

    req_id = 10
    ws_client.send(json.dumps({
        "jsonrpc": "2.0",
        "method": "color",
        "params": {"hsv": {"h": target_h, "s": 100, "v": 100}, "t": 100},
        "id": req_id,
    }))

    response = recv_ws_id(ws_client, req_id)
    step("WS cmd:color HSV returns a response with matching id", response is not None)
    step("WS response has no error", response is not None and "error" not in response,
         str(response.get("error") if response else "no response"))

    time.sleep(0.4)
    http_state = requests.get(f"{base_url}/color").json()
    h_http = float(http_state.get("hsv", {}).get("h", -1))
    step(
        f"GET /color confirms H ≈ {target_h}°",
        abs(h_http - target_h) < 5,
        f"H={h_http}",
    )


def test_ws_color_set_raw(ws_client, base_url):
    """WS cmd:color raw — r/g/b values are reflected in GET /color response"""
    req_id = 11
    target = {"r": 256, "g": 1023, "b": 0, "ww": 0, "cw": 0}
    ws_client.send(json.dumps({
        "jsonrpc": "2.0",
        "method": "color",
        "params": {"raw": target, "t": 100},
        "id": req_id,
    }))

    response = recv_ws_id(ws_client, req_id)
    step("WS cmd:color raw returns a response with matching id", response is not None)
    step("WS response has no error", response is not None and "error" not in response,
         str(response.get("error") if response else "no response"))

    time.sleep(0.4)
    http_state = requests.get(f"{base_url}/color").json()
    step("GET /color response includes 'raw' object", "raw" in http_state)
    got_raw = http_state.get("raw", {})
    for ch in ("r", "g", "b", "ww", "cw"):
        step(f"raw.{ch} matches sent value", got_raw.get(ch) == target[ch],
             f"expected {target[ch]}, got {got_raw.get(ch)}")


def test_ws_color_notification(ws_client, base_url):
    """WS cmd:color triggers a broadcast color_event notification"""
    req_id = 12
    target_h = 60  # yellow
    ws_client.send(json.dumps({
        "jsonrpc": "2.0",
        "method": "color",
        "params": {"hsv": {"h": target_h, "s": 100, "v": 100}, "t": 50},
        "id": req_id,
    }))

    # Consume the JSON-RPC response first so recv_notification sees only broadcasts
    recv_ws_id(ws_client, req_id)

    notification = recv_notification(ws_client, "color_event")
    step("WS color_event broadcast received after cmd:color", notification is not None)
    if notification:
        h_notif = float(notification.get("hsv", {}).get("h", -1))
        step(
            f"notification H ≈ {target_h}°",
            abs(h_notif - target_h) < 5,
            f"H={h_notif}",
        )
