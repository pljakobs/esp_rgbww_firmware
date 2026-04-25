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


def http_request(method: str, url: str, body: bytes | None = None, headers: dict[str, str] | None = None, timeout: float = 5.0, max_redirects: int = 5) -> dict[str, Any]:
    """Make an HTTP request and follow redirects up to max_redirects times."""
    for redirect_count in range(max_redirects):
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
