from __future__ import annotations

import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
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
    log_service_url: str
    log_service_port: int
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
        log_service_url=os.environ.get("HOST_SMOKE_LOG_SERVICE_URL", ""),
        log_service_port=int(os.environ.get("HOST_SMOKE_LOG_SERVICE_PORT", "4821")),
        log_dir=log_dir,
        http_trace_log=log_dir / "http-probes.jsonl",
        malformed_json_trace=log_dir / "malformed-color-response.json",
    )


@pytest.fixture(scope="session")
def api_secured(smoke_config: SmokeConfig) -> bool:
    response = http_request("GET", f"{smoke_config.base_url}/config")
    if response["error"] or response["status"] != 200:
        return False
    try:
        config = json.loads(response["body"])
    except json.JSONDecodeError:
        return False
    return bool(config.get("security", {}).get("api_secured", False))


@pytest.fixture(scope="session", autouse=True)
def remote_syslog_capture(smoke_config: SmokeConfig):
    if os.environ.get("HOST_SMOKE_CAPTURE_LOG_SERVICE") != "1":
        yield
        return

    config_response = http_request("GET", f"{smoke_config.base_url}/config")
    if config_response["error"] or config_response["status"] != 200:
        pytest.fail("Cannot enable test syslog capture: GET /config failed")
    try:
        config = json.loads(config_response["body"])
    except json.JSONDecodeError:
        pytest.fail("Cannot enable test syslog capture: GET /config was not JSON")
    if config.get("security", {}).get("api_secured", False):
        pytest.fail("Log-service capture does not support API-secured devices")

    rsyslog = config.get("network", {}).get("rsyslog", {})
    if not rsyslog.get("enabled") or not rsyslog.get("host"):
        pytest.fail("Controller remote syslog is not enabled or has no target host")

    service_url = smoke_config.log_service_url or (
        f"http://{rsyslog['host']}:{smoke_config.log_service_port}"
    )
    service_url = service_url.rstrip("/")
    try:
        health = fetch_log_service_json(f"{service_url}/api/v1/health")
    except Exception as exc:  # noqa: BLE001
        pytest.fail(f"Cannot reach log-service API at {service_url}: {exc}")
    if health.get("status") != "ok":
        pytest.fail(f"Log-service API is not healthy at {service_url}")

    def get_logs(from_id: int, limit: int = 2000) -> dict[str, Any]:
        query = parse.urlencode({"ip": smoke_config.app_ip, "from": from_id, "limit": limit})
        return fetch_log_service_json(f"{service_url}/api/v1/logs?{query}")

    try:
        latest = get_logs(0, limit=1)
    except Exception as exc:  # noqa: BLE001
        pytest.fail(f"Cannot read controller logs from log-service: {exc}")
    latest_items = latest.get("items", [])
    cursor = max((int(item["id"]) for item in latest_items), default=0) + 1

    syslog_capture_path = smoke_config.log_dir / "device-syslog.jsonl"
    syslog_capture_path.write_text("", encoding="utf-8")
    try:
        sys.__stdout__.write(f"[device-syslog] reading {service_url} for {smoke_config.app_ip}\n")
        sys.__stdout__.flush()
        probe = http_request("GET", smoke_config.color_url)
        if probe["error"] or probe["status"] != 200:
            pytest.fail("Read-only /color probe failed during syslog verification")
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if get_logs(cursor).get("items"):
                break
            time.sleep(0.25)
        else:
            pytest.fail(
                f"No new logs for {smoke_config.app_ip} appeared in the log-service after /color"
            )
        yield
    finally:
        next_from = cursor
        captured = 0
        while True:
            page = get_logs(next_from)
            items = page.get("items", [])
            for item in items:
                captured_at = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
                append_jsonl(syslog_capture_path, {"captured_at": captured_at, **item})
                sys.__stdout__.write(
                    f"[device-syslog {item.get('receivedAt', '')} {item.get('message', '')}]\n"
                )
                captured += 1
            sys.__stdout__.flush()
            next_from = page.get("nextAfter")
            if not next_from:
                break
        sys.__stdout__.write(f"[device-syslog] exported {captured} log entries to {syslog_capture_path}\n")
        sys.__stdout__.flush()


def fetch_log_service_json(url: str, timeout: float = 60.0) -> dict[str, Any]:
    req = request.Request(url, headers={"Accept": "application/json"})
    for attempt in range(4):
        try:
            with request.urlopen(req, timeout=timeout) as response:
                if response.status != 200:
                    raise RuntimeError(f"HTTP {response.status}")
                payload = json.loads(response.read().decode("utf-8"))
            break
        except error.HTTPError as exc:
            if attempt == 3 or exc.code not in (408, 429, 500, 502, 503, 504):
                raise
        except (error.URLError, TimeoutError, ConnectionError, http.client.IncompleteRead):
            if attempt == 3:
                raise
        time.sleep(1)
    if not isinstance(payload, dict):
        raise RuntimeError("API response was not a JSON object")
    return payload


@pytest.fixture(autouse=True)
def device_test_markers(request: pytest.FixtureRequest):
    record_device_request({"transport": "pytest", "method": "test-start", "test": request.node.nodeid})
    try:
        yield
    finally:
        record_device_request({"transport": "pytest", "method": "test-end", "test": request.node.nodeid})


def append_jsonl(path: Path, record: dict[str, Any]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, separators=(",", ":")) + "\n")


def record_device_request(record: dict[str, Any]) -> None:
    if os.environ.get("HOST_SMOKE_CAPTURE_LOG_SERVICE") != "1":
        return
    timestamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
    log_dir = Path(os.environ["HOST_SMOKE_LOG_DIR"])
    append_jsonl(log_dir / "device-request-markers.jsonl", {"sent_at": timestamp, **record})
    label = record.get("path") or record.get("method") or "request"
    sys.__stdout__.write(f"[device-request {timestamp}] {record['transport']} {label}\n")
    sys.__stdout__.flush()


def write_json(path: Path, payload: dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")


def http_request(method: str, url: str, body: bytes | None = None, headers: dict[str, str] | None = None, timeout: float = 5.0, max_redirects: int = 5) -> dict[str, Any]:
    """Make an HTTP request and follow redirects up to max_redirects times."""
    for redirect_count in range(max_redirects):
        record_device_request(
            {"transport": "HTTP", "method": method, "path": parse.urlsplit(url).path or "/"}
        )
        req = request.Request(url, data=body, headers=headers or {}, method=method)
        try:
            with request.urlopen(req, timeout=timeout) as response:
                # Check if this is a redirect (3xx status code)
                if 300 <= response.status < 400:
                    location = response.headers.get('Location')
                    if location:
                        url = location
                        # For redirects, follow with GET and no body
                        method = 'GET'
                        body = None
                        continue
                
                raw_body = response.read()
                return {
                    "status": response.status,
                    "reason": getattr(response, "reason", "") or "",
                    "headers": dict(response.getheaders()),
                    "body": raw_body.decode("utf-8", errors="replace"),
                    "error": "",
                }
        except error.HTTPError as exc:
            # Check if this is a redirect error (3xx status code)
            if 300 <= exc.code < 400:
                location = exc.headers.get('Location')
                if location:
                    url = location
                    # For redirects, follow with GET and no body
                    method = 'GET'
                    body = None
                    continue
            
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
    
    # Max redirects exceeded
    return {
        "status": 0,
        "reason": "",
        "headers": {},
        "body": "",
        "error": "Max redirects exceeded",
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
    try:
        payload_record = json.loads(text)
    except json.JSONDecodeError:
        payload_record = None
    if isinstance(payload_record, dict):
        record_device_request(
            {
                "transport": "WebSocket",
                "method": payload_record.get("method", ""),
                "id": payload_record.get("id"),
            }
        )
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


def get_color_restore_payload(smoke_config: SmokeConfig) -> dict[str, Any]:
    response = http_request("GET", smoke_config.color_url)
    if response["error"] or response["status"] != 200:
        pytest.fail("Could not snapshot color before API surface probe")
    try:
        color = json.loads(response["body"])
    except json.JSONDecodeError:
        pytest.fail("Could not parse color snapshot before API surface probe")
    for mode in ("raw", "hsv"):
        values = color.get(mode)
        if isinstance(values, dict):
            payload = {mode: values}
            if mode == "hsv":
                payload["cmd"] = "solid"
            return payload
    pytest.fail("Color snapshot did not include raw or hsv values")


def restore_color(smoke_config: SmokeConfig, payload: dict[str, Any]) -> None:
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    response = http_request(
        "POST",
        smoke_config.color_url,
        body=body,
        headers={"Content-Type": "application/json"},
    )
    if response["error"] or response["status"] != 200:
        pytest.fail("Could not restore raw color after API surface probe")


def recv_ws_text(sock: socket.socket, timeout: float = 3.0) -> str:
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


def ws_request(sock: socket.socket, payload: dict[str, Any], timeout: float = 3.0) -> tuple[dict[str, Any], dict[str, Any]]:
    ws_send_text(sock, json.dumps(payload, separators=(",", ":")))
    deadline = time.monotonic() + timeout
    ignored_frames: list[dict[str, Any]] = []
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            fail_with_trace(
                "Timed out waiting for the matching JSON-RPC response",
                {"request": payload, "response": {"notifications": ignored_frames}},
            )
        response_text = recv_ws_text(sock, timeout=remaining)
        try:
            response = json.loads(response_text)
        except json.JSONDecodeError:
            fail_with_trace(
                "WebSocket response was not valid JSON",
                {"request": payload, "response": {"text": response_text}},
            )
        trace = {"request": payload, "response": {"payload": response, "notifications": ignored_frames}}
        if response.get("id") == payload.get("id"):
            return response, trace
        ignored_frames.append(response)


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

    # Validate top-level payload structure directly:
    required_keys = {"version", "device", "app", "sming"}
    if not required_keys.issubset(payload.keys()):
        fail_with_trace("/info response missing expected top-level fields", trace)

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


def test_websocket_missing_method(smoke_config: SmokeConfig, api_secured: bool) -> None:
    if api_secured:
        pytest.skip("unauthenticated RPC probes are unavailable when API security is enabled")
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


def test_websocket_unknown_method(smoke_config: SmokeConfig, api_secured: bool) -> None:
    if api_secured:
        pytest.skip("unauthenticated RPC probes are unavailable when API security is enabled")
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before unknown-method probe", handshake_trace)
        probe = {
            "jsonrpc": "2.0",
            "id": 7002,
            "method": "definitelyNotAMethod",
        }
        payload, trace = ws_request(sock, probe, timeout=3.0)
        if payload.get("id") != 7002:
            fail_with_trace("Unexpected JSON-RPC id in unknown-method response", trace)
        assert_method_error_variant(jsonrpc_error_text(payload), trace)
    finally:
        sock.close()


@pytest.mark.parametrize(
    "path",
    ["/color", "/networks", "/hosts", "/config", "/connect", "/data", "/webapp_status", "/webapp_check"],
)
def test_http_json_read_surface(smoke_config: SmokeConfig, path: str) -> None:
    response = http_request("GET", f"{smoke_config.base_url}{path}")
    body_for_trace = "<redacted>" if path == "/config" else response["body"]
    trace = {
        "request": {"method": "GET", "path": path},
        "response": {"status": response["status"], "body": body_for_trace, "error": response["error"]},
    }
    if response["error"]:
        fail_with_trace(f"GET {path} failed", trace)
    try:
        payload = json.loads(response["body"])
    except json.JSONDecodeError:
        fail_with_trace(f"GET {path} did not return JSON", trace)
    if response["status"] != 200 or not isinstance(payload, (dict, list)):
        fail_with_trace(f"GET {path} should return a JSON object or array", trace)


@pytest.mark.parametrize(
    ("path", "body"),
    [
        ("/color", {"hsv": {"h": 31, "s": 60, "v": 55}, "cmd": "solid"}),
        ("/stop", {}),
        ("/skip", {}),
        ("/pause", {}),
        ("/continue", {}),
        ("/blink", {}),
        ("/toggle", {}),
        ("/on", {}),
        ("/off", {}),
        ("/scan_networks", {}),
        pytest.param(
            "/system",
            {"cmd": "debug", "enable": False},
            marks=pytest.mark.skipif(
                os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1",
                reason="changes the physical device's serial debug setting",
            ),
        ),
    ],
)
def test_http_command_surface(smoke_config: SmokeConfig, path: str, body: dict[str, Any]) -> None:
    url = f"{smoke_config.base_url}{path}"
    body_bytes = json.dumps(body, separators=(",", ":")).encode("utf-8")
    mutates_color = path in {"/color", "/blink", "/toggle", "/on", "/off"}
    original_color = get_color_restore_payload(smoke_config) if mutates_color else None
    try:
        response = http_request(
            "POST", url, body=body_bytes, headers={"Content-Type": "application/json"}
        )
        trace = {
            "request": {"method": "POST", "url": url, "body": body},
            "response": response,
        }
        payload = decode_json_response(response, trace, failure_message=f"POST {path} failed")
        if response["status"] != 200 or payload.get("success") is not True:
            fail_with_trace(f"POST {path} should return success", trace)
    finally:
        if path == "/blink":
            http_request(
                "POST",
                f"{smoke_config.base_url}/stop",
                body=b"{}",
                headers={"Content-Type": "application/json"},
            )
        if original_color is not None:
            restore_color(smoke_config, original_color)


def test_http_config_and_data_accept_empty_imports(smoke_config: SmokeConfig) -> None:
    for path in ("/config", "/data"):
        url = f"{smoke_config.base_url}{path}"
        response = http_request(
            "POST", url, body=b"{}", headers={"Content-Type": "application/json"}
        )
        trace = {
            "request": {"method": "POST", "url": url, "body": "{}"},
            "response": response,
        }
        payload = decode_json_response(response, trace, failure_message=f"POST {path} failed")
        if response["status"] != 200 or payload.get("success") is not True:
            fail_with_trace(f"POST {path} should accept an empty ConfigDB update", trace)


def test_http_connect_missing_ssid_is_rejected(smoke_config: SmokeConfig) -> None:
    url = f"{smoke_config.base_url}/connect"
    response = http_request(
        "POST",
        url,
        body=b'{"password":"unused"}',
        headers={"Content-Type": "application/json"},
    )
    trace = {
        "request": {"method": "POST", "url": url, "body": '{"password":"<redacted>"}'},
        "response": {"status": response["status"], "error": response["error"]},
    }
    if response["error"] or response["status"] != 400:
        fail_with_trace("POST /connect without ssid should be rejected", trace)


@pytest.mark.skipif(
    os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1",
    reason="POST /update can flash the physical device",
)
def test_http_update_is_host_unsupported(smoke_config: SmokeConfig) -> None:
    url = f"{smoke_config.base_url}/update"
    response = http_request(
        "POST",
        url,
        body=b'{"rom":{"url":"http://127.0.0.1/unused"}}',
        headers={"Content-Type": "application/json"},
    )
    trace = {"request": {"method": "POST", "url": url}, "response": response}
    if response["error"] or response["status"] != 400 or "not supported on Host" not in response["body"]:
        fail_with_trace("POST /update should report the Host-only limitation", trace)


@pytest.mark.parametrize(
    ("method", "params", "expected_keys"),
    [
        ("info", {}, {"version", "device", "app", "sming"}),
        ("getInfo", {"sparse": False}, {"version", "runtime", "debug"}),
        ("color", {}, {"raw", "hsv"}),
        ("getColor", {}, {"raw", "hsv"}),
        ("networks", {}, {"scanning", "available"}),
        ("getNetworks", {}, {"scanning", "available"}),
    ],
)
def test_websocket_query_surface(
    smoke_config: SmokeConfig,
    api_secured: bool,
    method: str,
    params: dict[str, Any],
    expected_keys: set[str],
) -> None:
    if api_secured:
        pytest.skip("query RPC tests require an authenticated WebSocket session")
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before query-surface probe", handshake_trace)
        request_id = 9000
        payload, trace = ws_request(
            sock,
            {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params},
        )
        result = payload.get("result")
        if payload.get("id") != request_id or not isinstance(result, dict):
            fail_with_trace(f"{method} should return a JSON-RPC result object", trace)
        color_result = method in {"color", "getColor"}
        has_color = isinstance(result.get("raw"), dict) or isinstance(result.get("hsv"), dict)
        if color_result and not has_color:
            fail_with_trace(f"{method} should return the active color representation", trace)
        if not color_result and not expected_keys.issubset(result):
            fail_with_trace(f"{method} result is missing expected fields", trace)
    finally:
        sock.close()


@pytest.mark.parametrize(
    ("method", "params"),
    [
        ("color", {"hsv": {"h": 31, "s": 60, "v": 55}, "cmd": "solid"}),
        ("direct", {"hsv": {"h": 41, "s": 70, "v": 45}}),
        ("stop", {"hsv": {"h": 31, "s": 60, "v": 55}, "cmd": "solid"}),
        ("skip", {"hsv": {"h": 31, "s": 60, "v": 55}, "cmd": "solid"}),
        ("pause", {"hsv": {"h": 31, "s": 60, "v": 55}, "cmd": "solid"}),
        ("continue", {}),
        ("blink", {}),
        ("toggle", {}),
        ("on", {}),
        ("setOn", {}),
        ("off", {}),
        ("setOff", {}),
        ("scan_networks", {}),
        pytest.param(
            "system",
            {"cmd": "debug", "enable": False},
            marks=pytest.mark.skipif(
                os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1",
                reason="changes the physical device's serial debug setting",
            ),
        ),
    ],
)
def test_websocket_command_surface(
    smoke_config: SmokeConfig, api_secured: bool, method: str, params: dict[str, Any]
) -> None:
    if api_secured:
        pytest.skip("command RPC tests require an authenticated WebSocket session")
    mutates_color = method in {
        "color", "direct", "stop", "skip", "pause", "blink", "toggle", "on", "setOn", "off", "setOff"
    }
    original_color = get_color_restore_payload(smoke_config) if mutates_color else None
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before command-surface probe", handshake_trace)
        request_id = 9100
        payload, trace = ws_request(
            sock,
            {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params},
        )
        if payload.get("id") != request_id or payload.get("result", {}).get("success") is not True:
            fail_with_trace(f"{method} should return JSON-RPC success", trace)
        if method == "blink":
            stop_id = request_id + 1
            stop_payload, stop_trace = ws_request(
                sock,
                {
                    "jsonrpc": "2.0",
                    "id": stop_id,
                    "method": "stop",
                    "params": {"hsv": {"h": 31, "s": 60, "v": 55}, "cmd": "solid"},
                },
            )
            if stop_payload.get("result", {}).get("success") is not True:
                fail_with_trace("stop should clear the blink command", stop_trace)
    finally:
        sock.close()
        if method == "blink":
            http_request(
                "POST",
                f"{smoke_config.base_url}/stop",
                body=b"{}",
                headers={"Content-Type": "application/json"},
            )
        if original_color is not None:
            restore_color(smoke_config, original_color)


def test_websocket_runtime_subscriptions(smoke_config: SmokeConfig, api_secured: bool) -> None:
    if api_secured:
        pytest.skip("subscription RPC tests require an authenticated WebSocket session")
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before subscription probes", handshake_trace)
        for request_id, method, expected in (
            (9160, "runtime_info_subscribe", True),
            (9161, "runtime_info_unsubscribe", False),
        ):
            payload, trace = ws_request(
                sock,
                {"jsonrpc": "2.0", "id": request_id, "method": method, "params": {}},
            )
            result = payload.get("result", {})
            if (
                payload.get("id") != request_id
                or result.get("subscribed") is not expected
                or result.get("channel") != "runtime_info"
            ):
                fail_with_trace(f"{method} should acknowledge the subscription state", trace)
    finally:
        sock.close()


def test_websocket_authenticate_entry(smoke_config: SmokeConfig) -> None:
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before auth probe", handshake_trace)
        request_id = 9150
        payload, trace = ws_request(
            sock,
            {"jsonrpc": "2.0", "id": request_id, "method": "authenticate", "params": {"hash": ""}},
        )
        if payload.get("id") != request_id:
            fail_with_trace("authenticate should preserve the JSON-RPC id", trace)
        if payload.get("result", {}).get("authenticated") is not True:
            error = payload.get("error", {})
            if error.get("code") != -32001 or not error.get("challenge"):
                fail_with_trace("authenticate should succeed or return a challenge", trace)
    finally:
        sock.close()


@pytest.mark.skipif(
    os.environ.get("HOST_SMOKE_ALLOW_OTA_TESTS") != "1",
    reason="POST /webapp_check and its RPC equivalent start a live OTA metadata request",
)
def test_webapp_check_acknowledges_async_http_and_rpc(smoke_config: SmokeConfig) -> None:
    url = f"{smoke_config.base_url}/webapp_check"
    response = http_request("POST", url)
    trace = {"request": {"method": "POST", "url": url}, "response": response}
    payload = decode_json_response(response, trace, failure_message="POST /webapp_check failed")
    if response["status"] != 200 or not isinstance(payload, dict):
        fail_with_trace("POST /webapp_check should return its status object", trace)

    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        if "101" not in handshake_trace["response"]["status_line"]:
            fail_with_trace("WebSocket handshake failed before webapp_check probe", handshake_trace)
        request_id = 9200
        rpc_payload, rpc_trace = ws_request(
            sock,
            {"jsonrpc": "2.0", "id": request_id, "method": "webapp_check", "params": {}},
        )
        if rpc_payload.get("id") != request_id or rpc_payload.get("result", {}).get("success") is not True:
            fail_with_trace("webapp_check should acknowledge the asynchronous check", rpc_trace)
    finally:
        sock.close()
