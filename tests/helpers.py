"""
Test helpers for esp_rgbww_firmware pytest suite.

Usage inside a test:
    from helpers import step, recv_stream

    step("GET /info returns 200", resp.status_code == 200)
    step("body has uptime key", "uptime" in data, detail=str(data.keys()))
"""

import json

_GREEN = "\033[32m"
_RED   = "\033[31m"
_RESET = "\033[0m"


def step(label: str, condition: bool, detail: str = "") -> None:
    """Print a single labelled check line and assert the condition.

    Output format (with -s):
        [OK  ] label  (optional detail)
        [FAIL] label  (optional detail)
    """
    tag    = f"{_GREEN}OK  {_RESET}" if condition else f"{_RED}FAIL{_RESET}"
    suffix = f"  ({detail})" if detail else ""
    print(f"  [{tag}] {label}{suffix}")
    assert condition, f"FAILED: {label}" + (f" — {detail}" if detail else "")


def recv_ws_id(ws_client, req_id: int, attempts: int = 20):
    """Receive messages until one with matching id arrives. Returns result field."""
    for _ in range(attempts):
        msg = ws_client.recv()
        if isinstance(msg, bytes):
            continue
        try:
            data = json.loads(msg)
        except json.JSONDecodeError:
            continue
        if data.get("id") == req_id:
            return data
    return None


def recv_stream(ws_client, req_id: int):
    """Receive a two-phase binary stream introduced by stream_start.

    Returns the reassembled bytes, or raises AssertionError on protocol error.
    """
    # Phase 1 — TEXT stream_start
    stream_start = None
    for _ in range(20):
        msg = ws_client.recv()
        if isinstance(msg, bytes):
            continue
        try:
            data = json.loads(msg)
        except json.JSONDecodeError:
            continue
        if data.get("id") == req_id and data.get("result", {}).get("type") == "stream_start":
            stream_start = data
            break

    assert stream_start is not None, f"Did not receive stream_start for id={req_id}"
    assert stream_start["result"].get("content-type") == "application/json", \
        "stream_start content-type should be application/json"

    # Phase 2 — BINARY chunks until TEXT stream_end
    chunks = []
    for _ in range(200):
        msg = ws_client.recv()
        if isinstance(msg, bytes):
            chunks.append(msg)
        else:
            try:
                data = json.loads(msg)
            except json.JSONDecodeError:
                continue
            if (data.get("method") == "stream_end" and
                    data.get("params", {}).get("id") == req_id):
                break

    assert len(chunks) > 0, f"No data chunks received for stream id={req_id}"
    return b"".join(chunks)


def recv_notification(ws_client, method: str, timeout_msgs: int = 30):
    """Wait for a WS broadcast notification with the given method name."""
    for _ in range(timeout_msgs):
        msg = ws_client.recv()
        if isinstance(msg, bytes):
            continue
        try:
            data = json.loads(msg)
        except json.JSONDecodeError:
            continue
        if data.get("method") == method:
            return data.get("params", data)
    return None
