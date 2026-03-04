import json
import pytest
import requests
import time
from helpers import step, recv_stream, recv_ws_id


def test_config_consistency(ws_client, base_url):
    """Verify /config is identical over HTTP and WS (streaming protocol)"""

    # HTTP
    http_resp = requests.get(f"{base_url}/config")
    step("GET /config returns HTTP 200", http_resp.status_code == 200, f"got {http_resp.status_code}")
    http_config = http_resp.json()
    step("HTTP /config is a non-empty JSON object", isinstance(http_config, dict) and len(http_config) > 0,
         f"{len(http_config)} keys")

    # WS streaming request
    req_id = 42
    ws_client.send(json.dumps({"jsonrpc": "2.0", "method": "config", "params": {}, "id": req_id}))
    raw = recv_stream(ws_client, req_id)
    step("WS stream_start received and BINARY chunks arrived", len(raw) > 0, f"{len(raw)} bytes")

    ws_config = json.loads(raw)
    step("reassembled stream parses as JSON object", isinstance(ws_config, dict))

    only_ws   = sorted(set(ws_config.keys()) - set(http_config.keys()))
    only_http = sorted(set(http_config.keys()) - set(ws_config.keys()))
    step(
        "WS and HTTP top-level keys match",
        not only_ws and not only_http,
        f"WS-only={only_ws}  HTTP-only={only_http}",
    )


def test_data_consistency(ws_client, base_url):
    """Verify /data is identical over HTTP and WS (streaming protocol)"""

    # HTTP
    http_resp = requests.get(f"{base_url}/data")
    step("GET /data returns HTTP 200", http_resp.status_code == 200, f"got {http_resp.status_code}")
    http_data = http_resp.json()
    step("HTTP /data is a non-empty JSON object", isinstance(http_data, dict) and len(http_data) > 0,
         f"{len(http_data)} keys")

    # WS streaming request
    req_id = 43
    ws_client.send(json.dumps({"jsonrpc": "2.0", "method": "data", "params": {}, "id": req_id}))
    raw = recv_stream(ws_client, req_id)
    step("WS stream_start received and BINARY chunks arrived", len(raw) > 0, f"{len(raw)} bytes")

    ws_data = json.loads(raw)
    step("reassembled stream parses as JSON object", isinstance(ws_data, dict))

    only_ws   = sorted(set(ws_data.keys()) - set(http_data.keys()))
    only_http = sorted(set(http_data.keys()) - set(ws_data.keys()))
    step(
        "WS and HTTP top-level keys match",
        not only_ws and not only_http,
        f"WS-only={only_ws}  HTTP-only={only_http}",
    )


def test_info_consistency(ws_client, base_url):
    """Verify /info (HTTP) matches cmd:info (WS) keys and static fields"""

    http_info = requests.get(f"{base_url}/info").json()
    step("GET /info returns a JSON object", isinstance(http_info, dict))

    req_id = 2
    ws_client.send(json.dumps({"jsonrpc": "2.0", "method": "info", "params": {}, "id": req_id}))
    response = recv_ws_id(ws_client, req_id)
    step("WS cmd:info returns a response with matching id", response is not None)
    ws_info = response.get("result", {})
    step("WS result is a non-empty object", isinstance(ws_info, dict) and len(ws_info) > 0)

    for key in ("deviceid", "build_type", "git_version", "sming"):
        if key in http_info:
            step(
                f"static field '{key}' matches",
                http_info[key] == ws_info.get(key),
                f"HTTP={http_info[key]!r}  WS={ws_info.get(key)!r}",
            )

    uptime_diff = abs(int(http_info.get("uptime", 0)) - int(ws_info.get("uptime", 0)))
    step("uptime values are within 10 s of each other", uptime_diff < 10, f"diff={uptime_diff}")


def test_color_consistency(ws_client, base_url):
    """Verify /color (HTTP) matches cmd:color (WS)"""

    # Stabilise to a known color first
    requests.post(f"{base_url}/color", json={"hsv": {"h": 120, "s": 100, "v": 100}})
    time.sleep(0.5)

    http_color = requests.get(f"{base_url}/color").json()
    step("GET /color returns hsv object", "hsv" in http_color)

    req_id = 3
    ws_client.send(json.dumps({"jsonrpc": "2.0", "method": "color", "params": {}, "id": req_id}))
    response = recv_ws_id(ws_client, req_id)
    step("WS cmd:color returns a response with matching id", response is not None)
    ws_color = response.get("result", {}) if response else {}
    step("WS result contains hsv", "hsv" in ws_color)

    h_http = float(http_color.get("hsv", {}).get("h", -1))
    h_ws   = float(ws_color.get("hsv",  {}).get("h", -2))
    step("hue (H) matches within 0.1°", abs(h_http - h_ws) < 0.1, f"HTTP={h_http}  WS={h_ws}")
