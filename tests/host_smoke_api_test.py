from __future__ import annotations

import base64
import hashlib
import http.client
import json
import os
import socket
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib import error, parse, request

import pytest


GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


@dataclass(frozen=True)
class SmokeConfig:
    app_ip: str
    base_url: str
    info_url: str
    color_url: str
    ws_host: str
    ws_port: int
    ws_path: str
    log_dir: Path
    http_trace_log: Path
    malformed_json_trace: Path


@pytest.fixture(scope="session")
def smoke_config() -> SmokeConfig:
    app_ip = os.environ["HOST_SMOKE_APP_IP"]
    base_url = os.environ.get("HOST_SMOKE_BASE_URL", f"http://{app_ip}")
    log_dir = Path(os.environ["HOST_SMOKE_LOG_DIR"])
    log_dir.mkdir(parents=True, exist_ok=True)

    return SmokeConfig(
        app_ip=app_ip,
        base_url=base_url,
        info_url=os.environ.get("HOST_SMOKE_INFO_URL", f"{base_url}/info"),
        color_url=os.environ.get("HOST_SMOKE_COLOR_URL", f"{base_url}/color"),
        ws_host=os.environ.get("HOST_SMOKE_WS_HOST", app_ip),
        ws_port=int(os.environ.get("HOST_SMOKE_WS_PORT", "80")),
        ws_path=os.environ.get("HOST_SMOKE_WS_PATH", "/ws"),
        log_dir=log_dir,
        http_trace_log=log_dir / "http-probes.jsonl",
        malformed_json_trace=log_dir / "malformed-color-response.json",
    )


def append_jsonl(path: Path, record: dict[str, Any]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, separators=(",", ":")) + "\n")


def write_json(path: Path, payload: dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")


def http_request(method: str, url: str, body: bytes | None = None, headers: dict[str, str] | None = None, timeout: float = 5.0) -> dict[str, Any]:
    req = request.Request(url, data=body, headers=headers or {}, method=method)
    try:
        with request.urlopen(req, timeout=timeout) as response:
            raw_body = response.read()
            return {
                "status": response.status,
                "reason": getattr(response, "reason", "") or "",
                "headers": dict(response.getheaders()),
                "body": raw_body.decode("utf-8", errors="replace"),
                "error": "",
            }
    except error.HTTPError as exc:
        try:
            raw_body = exc.read()
        except http.client.IncompleteRead as incomplete:
            raw_body = incomplete.partial or b""
        return {
            "status": exc.code,
            "reason": getattr(exc, "reason", "") or "",
            "headers": dict(exc.headers.items()),
            "body": raw_body.decode("utf-8", errors="replace"),
            "error": "",
        }
    except Exception as exc:  # noqa: BLE001
        return {
            "status": 0,
            "reason": "",
            "headers": {},
            "body": "",
            "error": repr(exc),
        }


def record_http_probe(
    smoke_config: SmokeConfig,
    *,
    probe: str,
    method: str,
    url: str,
    body: bytes | None,
    headers: dict[str, str],
    response: dict[str, Any],
    detail_path: Path | None = None,
) -> dict[str, Any]:
    trace = {
        "probe": probe,
        "request": {
            "method": method,
            "url": url,
            "path": parse.urlsplit(url).path or "/",
            "headers": headers,
            "body_utf8": (body or b"").decode("utf-8", errors="replace"),
            "body_hex": (body or b"").hex(),
            "body_length": len(body or b""),
        },
        "response": response,
    }
    append_jsonl(smoke_config.http_trace_log, trace)
    if detail_path is not None:
        write_json(detail_path, trace)
    return trace


def recv_until(sock: socket.socket, marker: bytes, timeout: float = 5.0) -> bytes:
    sock.settimeout(timeout)
    data = b""
    while marker not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    return data


def ws_recv_frame(sock: socket.socket, timeout: float = 2.0) -> tuple[int | None, bytes | None]:
    sock.settimeout(timeout)
    header = sock.recv(2)
    if not header:
        return None, None
    first, second = header
    opcode = first & 0x0F
    size = second & 0x7F
    if size == 126:
        size = struct.unpack("!H", sock.recv(2))[0]
    elif size == 127:
        size = struct.unpack("!Q", sock.recv(8))[0]
    payload = b""
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            break
        payload += chunk
    return opcode, payload


def ws_send_text(sock: socket.socket, text: str) -> None:
    payload = text.encode("utf-8")
    size = len(payload)
    if size < 126:
        header = bytes([0x81, 0x80 | size])
    elif size < (1 << 16):
        header = bytes([0x81, 0x80 | 126]) + struct.pack("!H", size)
    else:
        header = bytes([0x81, 0x80 | 127]) + struct.pack("!Q", size)
    mask = os.urandom(4)
    masked_payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    sock.sendall(header + mask + masked_payload)


def websocket_handshake(smoke_config: SmokeConfig, nonce_bytes: bytes) -> tuple[socket.socket, dict[str, Any]]:
    nonce = base64.b64encode(nonce_bytes).decode("ascii")
    handshake_request = (
        f"GET {smoke_config.ws_path} HTTP/1.1\r\n"
        f"Host: {smoke_config.ws_host}:{smoke_config.ws_port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {nonce}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock = socket.create_connection((smoke_config.ws_host, smoke_config.ws_port), timeout=5)
    sock.sendall(handshake_request.encode("ascii"))
    response_text = recv_until(sock, b"\r\n\r\n", timeout=5).decode("latin-1", errors="replace")
    status_line = response_text.split("\r\n", 1)[0].strip()
    headers = {}
    for line in response_text.split("\r\n")[1:]:
        if not line:
            break
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()

    expected_accept = base64.b64encode(
        hashlib.sha1((nonce + GUID).encode("ascii")).digest()
    ).decode("ascii")

    trace = {
        "probe": "websocket_handshake",
        "request": {
            "method": "GET",
            "path": smoke_config.ws_path,
            "headers": {
                "Host": f"{smoke_config.ws_host}:{smoke_config.ws_port}",
                "Upgrade": "websocket",
                "Connection": "Upgrade",
                "Sec-WebSocket-Key": nonce,
                "Sec-WebSocket-Version": "13",
            },
        },
        "response": {
            "status_line": status_line,
            "headers": headers,
            "raw": response_text,
            "expected_accept": expected_accept,
        },
    }
    return sock, trace


def fail_with_trace(message: str, trace: dict[str, Any], detail_path: Path | None = None) -> None:
    location = f"\ntrace_file: {detail_path}" if detail_path is not None else ""
    pytest.fail(
        message
        + "\ninput: "
        + json.dumps(trace.get("request", {}), ensure_ascii=True, sort_keys=True)
        + "\nresponse: "
        + json.dumps(trace.get("response", {}), ensure_ascii=True, sort_keys=True)
        + location
    )


def decode_json_response(response: dict[str, Any], trace: dict[str, Any], *, failure_message: str) -> Any:
    if response["error"]:
        fail_with_trace(failure_message, trace)
    try:
        return json.loads(response["body"]) if response["body"] else {}
    except json.JSONDecodeError:
        fail_with_trace(f"{failure_message}: invalid JSON body", trace)


def recv_ws_text(sock: socket.socket, timeout: float = 3.0) -> str:
    deadline = socket.getdefaulttimeout()
    del deadline
    while True:
        opcode, payload = ws_recv_frame(sock, timeout=timeout)
        if opcode is None:
            pytest.fail("WebSocket connection closed before a text frame was received")
        if opcode == 1:
            return (payload or b"").decode("utf-8", errors="replace")
        if opcode == 9:
            pong_payload = payload or b""
            sock.sendall(bytes([0x8A, len(pong_payload)]) + pong_payload)
            continue
        if opcode == 8:
            pytest.fail("Unexpected WebSocket close frame received")
        pytest.fail(f"Unsupported WebSocket opcode received: {opcode}")


def recv_ws_json(sock: socket.socket, timeout: float = 3.0) -> dict[str, Any]:
    text = recv_ws_text(sock, timeout=timeout)
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        pytest.fail(f"Expected JSON WebSocket payload, got {text!r}: {exc}")


def jsonrpc_error_text(payload: dict[str, Any]) -> str:
    error_value = payload.get("error", "")
    if isinstance(error_value, dict):
        return f"{error_value.get('message', '')} {error_value.get('code', '')}".strip()
    return str(error_value)


def assert_method_error_variant(message: str, trace: dict[str, Any]) -> None:
    normalized = message.lower()
    if (
        "missing method" not in normalized
        and "method not implemented" not in normalized
        and "method not found" not in normalized
        and "-32601" not in normalized
    ):
        fail_with_trace("Unexpected JSON-RPC method error", trace)


def make_distinct_raw(candidate: dict[str, Any], current: dict[str, Any] | None) -> dict[str, int]:
    base = current or {}
    normalized = {
        "r": int(candidate.get("r", 0)),
        "g": int(candidate.get("g", 0)),
        "b": int(candidate.get("b", 0)),
        "ww": int(candidate.get("ww", 0)),
        "cw": int(candidate.get("cw", 0)),
    }
    current_normalized = {
        key: int(base.get(key, 0)) for key in ["r", "g", "b", "ww", "cw"]
    }
    if normalized != current_normalized:
        return normalized

    normalized["r"] = (current_normalized["r"] + 1) % 256
    return normalized


def wait_for_color_raw(smoke_config: SmokeConfig, expected_raw: dict[str, int], attempts: int = 20) -> tuple[dict[str, Any], dict[str, Any]]:
    last_response: dict[str, Any] | None = None
    last_trace: dict[str, Any] | None = None
    for _ in range(attempts):
        response = http_request("GET", smoke_config.color_url)
        trace = record_http_probe(
            smoke_config,
            probe="color_poll",
            method="GET",
            url=smoke_config.color_url,
            body=None,
            headers={"Accept": "application/json"},
            response=response,
        )
        payload = decode_json_response(response, trace, failure_message="/color polling failed")
        last_response = payload
        last_trace = trace
        if payload.get("raw") == expected_raw:
            return payload, trace
    fail_with_trace(
        f"/color did not converge to expected raw value {expected_raw}",
        last_trace or {"request": {}, "response": last_response or {}},
    )


def ws_request(sock: socket.socket, payload: dict[str, Any], timeout: float = 3.0) -> tuple[dict[str, Any], dict[str, Any]]:
    ws_send_text(sock, json.dumps(payload, separators=(",", ":")))
    response_text = recv_ws_text(sock, timeout=timeout)
    trace = {
        "request": payload,
        "response": {"text": response_text},
    }
    try:
        return json.loads(response_text), trace
    except json.JSONDecodeError:
        fail_with_trace("WebSocket response was not valid JSON", trace)


def ws_wait_for_method(sock: socket.socket, method_name: str, timeout: float = 8.0) -> tuple[dict[str, Any], dict[str, Any]]:
    deadline = timeout
    while deadline > 0:
        response_text = recv_ws_text(sock, timeout=min(1.0, deadline))
        deadline -= 1.0
        trace = {
            "request": {"expected_method": method_name},
            "response": {"text": response_text},
        }
        try:
            payload = json.loads(response_text)
        except json.JSONDecodeError:
            fail_with_trace("WebSocket event payload was not valid JSON", trace)
        if payload.get("method") == method_name:
            return payload, trace
    pytest.fail(f"Timed out waiting for WebSocket method event {method_name!r}")


def test_websocket_handshake(smoke_config: SmokeConfig) -> None:
    sock, trace = websocket_handshake(smoke_config, b"host-ci-websocket-key")
    try:
        status_line = trace["response"]["status_line"]
        if "101" not in status_line:
            fail_with_trace("WebSocket handshake failed", trace)

        actual_accept = trace["response"]["headers"].get("sec-websocket-accept", "")
        expected_accept = trace["response"]["expected_accept"]
        if actual_accept != expected_accept:
            fail_with_trace("Invalid Sec-WebSocket-Accept header", trace)
    finally:
        sock.close()


def test_ping_endpoint(smoke_config: SmokeConfig) -> None:
    url = f"{smoke_config.base_url}/ping"
    response = http_request("GET", url)
    trace = record_http_probe(
        smoke_config,
        probe="ping",
        method="GET",
        url=url,
        body=None,
        headers={"Accept": "application/json"},
        response=response,
    )
    payload = decode_json_response(response, trace, failure_message="/ping probe failed")
    if response["status"] != 200 or payload.get("ping") != "pong":
        fail_with_trace("Unexpected /ping response", trace)


def test_info_endpoint(smoke_config: SmokeConfig) -> None:
    response = http_request("GET", smoke_config.info_url)
    trace = record_http_probe(
        smoke_config,
        probe="info",
        method="GET",
        url=smoke_config.info_url,
        body=None,
        headers={"Accept": "application/json"},
        response=response,
    )
    if response["error"]:
        fail_with_trace("/info probe failed unexpectedly", trace)
    if response["status"] != 200:
        fail_with_trace("/info did not return HTTP 200", trace)

    try:
        payload = json.loads(response["body"])
    except json.JSONDecodeError:
        fail_with_trace("/info returned invalid JSON", trace)

    required_keys = ["git_version", "build_type", "sming", "connection"]
    missing_keys = [key for key in required_keys if key not in payload]
    if missing_keys:
        fail_with_trace(f"Missing keys in /info response: {', '.join(missing_keys)}", trace)

    actual_ip = payload.get("connection", {}).get("ip")
    if actual_ip != smoke_config.app_ip:
        fail_with_trace(f"Unexpected Host IP in /info: {actual_ip!r} != {smoke_config.app_ip!r}", trace)

    write_json(smoke_config.log_dir / "info.json", payload)


def test_malformed_json_returns_bad_request(smoke_config: SmokeConfig) -> None:
    bad_payload = b'{"raw":{"r":12,"g":34'
    response = http_request(
        "POST",
        smoke_config.color_url,
        body=bad_payload,
        headers={"Content-Type": "application/json"},
    )
    trace = record_http_probe(
        smoke_config,
        probe="malformed_color_json",
        method="POST",
        url=smoke_config.color_url,
        body=bad_payload,
        headers={"Content-Type": "application/json"},
        response=response,
        detail_path=smoke_config.malformed_json_trace,
    )

    if response["error"]:
        fail_with_trace("Malformed JSON HTTP probe failed unexpectedly", trace, smoke_config.malformed_json_trace)
    if response["status"] != 400:
        fail_with_trace("Malformed JSON should return HTTP 400", trace, smoke_config.malformed_json_trace)
    if "Invalid JSON" not in response["body"]:
        fail_with_trace("Malformed JSON error body missing expected text", trace, smoke_config.malformed_json_trace)


@pytest.mark.parametrize("path", ["/color", "/on"])
def test_http_malformed_json_rejected(smoke_config: SmokeConfig, path: str) -> None:
    url = f"{smoke_config.base_url}{path}"
    bad_payload = b"{bad json"
    response = http_request(
        "POST",
        url,
        body=bad_payload,
        headers={"Content-Type": "application/json"},
    )
    trace = record_http_probe(
        smoke_config,
        probe=f"malformed_json_{path.strip('/').replace('/', '_')}",
        method="POST",
        url=url,
        body=bad_payload,
        headers={"Content-Type": "application/json"},
        response=response,
    )
    if response["error"]:
        fail_with_trace("Malformed JSON HTTP probe failed unexpectedly", trace)
    if response["status"] != 400 or "Invalid JSON" not in response["body"]:
        fail_with_trace("Malformed JSON request should be rejected with Invalid JSON", trace)


@pytest.mark.parametrize("path", ["/color", "/networks", "/hosts", "/config", "/connect", "/data"])
def test_get_endpoints_return_json(smoke_config: SmokeConfig, path: str) -> None:
    url = f"{smoke_config.base_url}{path}"
    response = http_request("GET", url)
    trace = record_http_probe(
        smoke_config,
        probe=f"get_{path.strip('/').replace('/', '_')}",
        method="GET",
        url=url,
        body=None,
        headers={"Accept": "application/json"},
        response=response,
    )
    payload = decode_json_response(response, trace, failure_message=f"GET {path} failed")
    if response["status"] != 200 or not isinstance(payload, (dict, list)):
        fail_with_trace(f"GET {path} should return JSON", trace)


def test_hosts_endpoint_returns_object(smoke_config: SmokeConfig) -> None:
    url = f"{smoke_config.base_url}/hosts"
    response = http_request("GET", url)
    trace = record_http_probe(
        smoke_config,
        probe="hosts_object",
        method="GET",
        url=url,
        body=None,
        headers={"Accept": "application/json"},
        response=response,
    )
    payload = decode_json_response(response, trace, failure_message="GET /hosts failed")
    if response["status"] != 200 or not isinstance(payload, dict):
        fail_with_trace("/hosts should return a JSON object", trace)


@pytest.mark.parametrize(
    ("path", "body"),
    [
        ("/scan_networks", {}),
        ("/system", {"cmd": "debug", "enable": False}),
        ("/stop", {}),
        ("/skip", {}),
        ("/pause", {}),
        ("/continue", {}),
        ("/blink", {}),
        ("/toggle", {}),
    ],
)
def test_post_action_endpoints_succeed(smoke_config: SmokeConfig, path: str, body: dict[str, Any]) -> None:
    url = f"{smoke_config.base_url}{path}"
    body_bytes = json.dumps(body, separators=(",", ":")).encode("utf-8")
    response = http_request(
        "POST",
        url,
        body=body_bytes,
        headers={"Content-Type": "application/json"},
    )
    trace = record_http_probe(
        smoke_config,
        probe=f"post_{path.strip('/').replace('/', '_')}",
        method="POST",
        url=url,
        body=body_bytes,
        headers={"Content-Type": "application/json"},
        response=response,
    )
    payload = decode_json_response(response, trace, failure_message=f"POST {path} failed")
    if response["status"] != 200 or payload.get("success") is not True:
        fail_with_trace(f"POST {path} should return success", trace)


def test_update_post_returns_api_error(smoke_config: SmokeConfig) -> None:
    url = f"{smoke_config.base_url}/update"
    body_bytes = b"{}"
    response = http_request(
        "POST",
        url,
        body=body_bytes,
        headers={"Content-Type": "application/json"},
    )
    trace = record_http_probe(
        smoke_config,
        probe="post_update",
        method="POST",
        url=url,
        body=body_bytes,
        headers={"Content-Type": "application/json"},
        response=response,
    )
    payload = decode_json_response(response, trace, failure_message="POST /update failed")
    if response["status"] != 400 or "error" not in payload:
        fail_with_trace("POST /update should return a 400 API error", trace)


def test_websocket_rejects_malformed_json(smoke_config: SmokeConfig) -> None:
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before malformed-json probe", handshake_trace)
        bad_request = "{bad json"
        ws_send_text(sock, bad_request)
        response_text = recv_ws_text(sock, timeout=3.0)
        trace = {
            "request": {"payload": bad_request},
            "response": {"text": response_text},
        }
        if "malformed json" not in response_text.lower():
            fail_with_trace("Expected malformed JSON rejection over WebSocket", trace)
    finally:
        sock.close()


def test_websocket_missing_method(smoke_config: SmokeConfig) -> None:
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before missing-method probe", handshake_trace)
        payload, trace = ws_request(sock, {"jsonrpc": "2.0", "id": 7001, "params": {}}, timeout=3.0)
        if payload.get("id") != 7001:
            fail_with_trace("Unexpected JSON-RPC id in missing-method response", trace)
        assert_method_error_variant(jsonrpc_error_text(payload), trace)
    finally:
        sock.close()


def test_websocket_unknown_method(smoke_config: SmokeConfig) -> None:
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        status_line = handshake_trace["response"]["status_line"]
        if "101" not in status_line:
            fail_with_trace("WebSocket handshake failed before unknown-method probe", handshake_trace)

        probe = {
            "jsonrpc": "2.0",
            "id": 7001,
            "method": "definitelyNotAMethod",
        }
        payload, trace = ws_request(sock, probe, timeout=3.0)
        if payload.get("id") != 7001:
            fail_with_trace("Unexpected JSON-RPC id in unknown-method response", trace)
        assert_method_error_variant(jsonrpc_error_text(payload), trace)
    finally:
        sock.close()


def test_color_setters_roundtrip_between_http_and_websocket(smoke_config: SmokeConfig) -> None:
    color_response = http_request("GET", smoke_config.color_url)
    color_trace = record_http_probe(
        smoke_config,
        probe="baseline_color",
        method="GET",
        url=smoke_config.color_url,
        body=None,
        headers={"Accept": "application/json"},
        response=color_response,
    )
    color_payload = decode_json_response(color_response, color_trace, failure_message="GET /color failed")
    base_raw = color_payload.get("raw")
    if not isinstance(base_raw, dict):
        fail_with_trace("GET /color should include a raw object", color_trace)

    target_http_raw = make_distinct_raw({"r": 12, "g": 34, "b": 56, "ww": 78, "cw": 90}, base_raw)
    http_body = json.dumps({"raw": target_http_raw}, separators=(",", ":")).encode("utf-8")
    set_response = http_request(
        "POST",
        smoke_config.color_url,
        body=http_body,
        headers={"Content-Type": "application/json"},
    )
    set_trace = record_http_probe(
        smoke_config,
        probe="set_color_http",
        method="POST",
        url=smoke_config.color_url,
        body=http_body,
        headers={"Content-Type": "application/json"},
        response=set_response,
    )
    set_payload = decode_json_response(set_response, set_trace, failure_message="POST /color failed")
    if set_response["status"] != 200 or set_payload.get("success") is not True:
        fail_with_trace("HTTP color setter should return success", set_trace)

    http_after_set, _ = wait_for_color_raw(smoke_config, target_http_raw)

    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before color roundtrip checks", handshake_trace)

        ws_get_payload, ws_get_trace = ws_request(
            sock,
            {"jsonrpc": "2.0", "id": 8001, "method": "getColor", "params": {}},
            timeout=3.0,
        )
        if ws_get_payload.get("result", {}).get("raw") != http_after_set.get("raw"):
            fail_with_trace("HTTP-set color differs between HTTP and WebSocket reads", ws_get_trace)

        target_ws_raw = make_distinct_raw({"r": 90, "g": 10, "b": 20, "ww": 30, "cw": 40}, http_after_set.get("raw"))
        ws_set_payload, ws_set_trace = ws_request(
            sock,
            {"jsonrpc": "2.0", "id": 8002, "method": "color", "params": {"raw": target_ws_raw}},
            timeout=3.0,
        )
        if ws_set_payload.get("result", {}).get("success") is not True:
            fail_with_trace("WebSocket color setter should return success", ws_set_trace)

        http_after_ws_set, _ = wait_for_color_raw(smoke_config, target_ws_raw)

        ws_after_payload, ws_after_trace = ws_request(
            sock,
            {"jsonrpc": "2.0", "id": 8003, "method": "getColor", "params": {}},
            timeout=3.0,
        )
        if ws_after_payload.get("result", {}).get("raw") != http_after_ws_set.get("raw"):
            fail_with_trace("WebSocket-set color differs between HTTP and WebSocket reads", ws_after_trace)
    finally:
        sock.close()


def test_off_on_emit_color_events(smoke_config: SmokeConfig) -> None:
    prep_raw = {"r": 5, "g": 10, "b": 15, "ww": 20, "cw": 25}
    prep_body = json.dumps({"raw": prep_raw}, separators=(",", ":")).encode("utf-8")
    prep_response = http_request(
        "POST",
        smoke_config.color_url,
        body=prep_body,
        headers={"Content-Type": "application/json"},
    )
    prep_trace = record_http_probe(
        smoke_config,
        probe="prep_non_zero_color",
        method="POST",
        url=smoke_config.color_url,
        body=prep_body,
        headers={"Content-Type": "application/json"},
        response=prep_response,
    )
    prep_payload = decode_json_response(prep_response, prep_trace, failure_message="Preparing non-zero color failed")
    if prep_response["status"] != 200 or prep_payload.get("success") is not True:
        fail_with_trace("Preparing non-zero color should succeed", prep_trace)
    wait_for_color_raw(smoke_config, prep_raw)

    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before on/off event checks", handshake_trace)

        for path in ["/off", "/on"]:
            url = f"{smoke_config.base_url}{path}"
            response = http_request(
                "POST",
                url,
                body=b"{}",
                headers={"Content-Type": "application/json"},
            )
            trace = record_http_probe(
                smoke_config,
                probe=f"post_{path.strip('/')}",
                method="POST",
                url=url,
                body=b"{}",
                headers={"Content-Type": "application/json"},
                response=response,
            )
            payload = decode_json_response(response, trace, failure_message=f"POST {path} failed")
            if response["status"] != 200 or payload.get("success") is not True:
                fail_with_trace(f"POST {path} should return success", trace)

            event_payload, event_trace = ws_wait_for_method(sock, "color_event", timeout=8.0)
            raw = event_payload.get("params", {}).get("raw", {})
            if path == "/off":
                if any(int(raw.get(channel, 0)) != 0 for channel in ["r", "g", "b", "ww", "cw"]):
                    fail_with_trace("/off should emit a zeroed color_event", event_trace)
            else:
                if all(int(raw.get(channel, 0)) == 0 for channel in ["r", "g", "b", "ww", "cw"]):
                    fail_with_trace("/on should emit a non-zero color_event", event_trace)
    finally:
        sock.close()


@pytest.mark.parametrize("path", ["/", "/webapp"])
def test_webapp_routes_respond(smoke_config: SmokeConfig, path: str) -> None:
    url = f"{smoke_config.base_url}{path}"
    response = http_request("GET", url)
    trace = record_http_probe(
        smoke_config,
        probe=f"webapp_{path.strip('/') or 'root'}",
        method="GET",
        url=url,
        body=None,
        headers={"Accept": "text/html"},
        response=response,
    )
    if response["error"]:
        fail_with_trace(f"GET {path} failed unexpectedly", trace)
    if response["status"] != 200:
        fail_with_trace(f"GET {path} should return HTTP 200", trace)

    body = response["body"]
    lowered = body.lower()
    if not any(marker in lowered for marker in ["<!doctype", "<html"]):
        if len(body) == 0 or (len(body) < 100 and "error" in lowered):
            fail_with_trace(f"GET {path} did not return a usable webapp response", trace)