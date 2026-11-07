"""
Created on 02.04.2017

@author: Robin

Converted to pytest format integrated with host_smoke_api_test.py harness by pjakobs
"""
from __future__ import annotations

import json
import os
import socket
import time
from typing import Optional
from urllib import error, request

import pytest

# Import helpers from host_smoke_api_test
from host_smoke_api_test import (
    SmokeConfig,
    api_secured,
    http_request,
    post_config_update,
    smoke_config,
    websocket_handshake,
    ws_recv_frame,
    ws_send_text,
)  # noqa: F401

TIME_SCALE = float(os.environ.get("RGBWW_TEST_TIME_SCALE", "1.0"))
RAMP_ACCURACY_RAMP_SECONDS = float(os.environ.get("RGBWW_RAMP_ACCURACY_SECONDS", "60"))
RAMP_ACCURACY_ITERATIONS = int(os.environ.get("RGBWW_RAMP_ACCURACY_ITERATIONS", "9"))
ASSERT_TOLERANCE = float(os.environ.get("RGBWW_TEST_TOLERANCE", "5.0"))
RETRY_READ_ATTEMPTS = int(os.environ.get("RGBWW_TEST_READ_RETRIES", "5"))
RETRY_READ_INTERVAL_SECONDS = float(os.environ.get("RGBWW_TEST_READ_RETRY_INTERVAL", "0.02"))

# Buffer to ensure final animation step (step 100) completes before sampling (~20ms per update cycle)
ANIMATION_COMPLETION_BUFFER_MS = 50  # 2-3 update cycles


def scaled(seconds: float) -> float:
    return max(0.0, seconds * TIME_SCALE)


def wait_for_fade_complete(fade_duration_s: float) -> None:
    """Wait for fade to complete plus buffer for final step execution"""
    time.sleep(fade_duration_s + ANIMATION_COMPLETION_BUFFER_MS / 1000.0)


def assert_eventually_close(
    read_fn,
    expected: float,
    tolerance: float,
    message: str,
    attempts: int = RETRY_READ_ATTEMPTS,
    interval_s: float = RETRY_READ_INTERVAL_SECONDS,
) -> None:
    """Retry a numeric read a few times to avoid transient early-read failures."""
    last_value = None
    for attempt in range(max(1, attempts)):
        last_value = read_fn()
        if abs(last_value - expected) < tolerance:
            return
        if attempt < attempts - 1:
            time.sleep(interval_s)
    assert False, f"{message} (after {attempts} reads/{interval_s:.3f}s), got {last_value}"

# JSON templates for color commands
jsonTempl = """{{
  "q":"{queue}",
  "hsv":{{"ct":2700,"v":"{val}","h":"{hue}","s":"{sat}"}},
  "d":1,
  "t":{time},
  "cmd":"{cmd}"
}}
"""

jsonTemplChannels = """{{"channels":[{channels}]}}"""

jsonTemplSolid = """{{
  "hsv":{{"ct":2700,"v":"{v}","h":"{h}","s":"{s}"}},
  "d":1,
  "cmd":"solid"
}}
"""

LIGHT_PROFILES = [
    ("RGBWWCW", [5, 4, 1, 0, 3], {"h": "120", "s": "80", "v": "70", "ct": "3000"}, ["r", "g", "b", "ww", "cw"], "v"),
    ("RGBWW", [5, 4, 1, 0], {"h": "120", "s": "80", "v": "70"}, ["r", "g", "b", "ww"], "v"),
    ("RGBCW", [5, 4, 1, 3], {"h": "120", "s": "80", "v": "70"}, ["r", "g", "b", "cw"], "v"),
    ("RGB", [5, 4, 1], {"h": "120", "s": "80", "v": "70"}, ["r", "g", "b"], "v"),
    ("WWCW", [5, 4], {"v": "70", "ct": "3000"}, ["ww", "cw"], "v"),
    ("WW", [5], {"v": "70"}, [], "v"),
    ("CW", [5], {"v": "70"}, [], "v"),
    ("SINGLE", [5], {"v": "70"}, [], "v"),
]
HSVCT_INPUT = {"h": "120", "s": "80", "v": "70", "ct": "3000"}


def _configuration(lights: list[dict]) -> dict:
    return {
        "board_id": "quinled-an-penta-mini",
        "board_revision": 1,
        "soc": "esp32c3",
        "system": {
            "clear_config_enabled": False,
            "clear_config_pin": 0,
            "clear_config_active_low": True,
            "clear_config_external_pull_up": False,
            "clear_config_external_pull_down": False,
        },
        "buttons": [],
        "buses": [],
        "lights": lights,
    }


def _light(light_type: str, pins: list[int], index: int, is_default: bool) -> dict:
    return {
        "id": f"rgbww-test-{index}",
        "index": index,
        "name": f"RGBWW test {index}",
        "is_default": is_default,
        "type": light_type,
        "pins": pins,
    }


def _configure_lights(smoke_config: SmokeConfig, lights: list[dict]) -> None:
    post_config_update(smoke_config, {"hardware": {"configuration": _configuration(lights)}})
    if not lights:
        return
    selector = next((light["index"] for light in lights if light["is_default"]), lights[0]["index"])
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        response = http_request("GET", f"{smoke_config.color_url}?light={selector}")
        if response["status"] == 200:
            return
        time.sleep(0.05)
    pytest.fail(f"Configured light {selector} did not become available: {response}")


@pytest.fixture(scope="module", autouse=True)
def configured_rgbww_suite(smoke_config: SmokeConfig, api_secured: bool):
    if os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1":
        yield
        return
    if api_secured:
        pytest.skip("RGBWW configuration tests require an unsecured Host instance")
    response = http_request("GET", f"{smoke_config.base_url}/config")
    if response["status"] != 200:
        pytest.fail(f"Could not snapshot hardware configuration: {response}")
    original = json.loads(response["body"])["hardware"]["configuration"]
    try:
        _configure_lights(smoke_config, [_light("RGBWW", [5, 4, 1, 0], 10, True)])
        yield
    finally:
        post_config_update(smoke_config, {"hardware": {"configuration": original}})


def do_post(base_url: str, endpoint: str, post_data: str) -> None:
    """Post data to endpoint, raise on error"""
    response = http_request(
        "POST",
        f"{base_url}/{endpoint}",
        body=post_data.encode(),
        headers={"Content-Type": "application/json"},
    )
    if response["status"] != 200:
        raise Exception(f"POST {endpoint} failed: {response['status']} {response['body']}")


def get_color(color_url: str) -> dict:
    """Fetch current color state"""
    response = http_request("GET", color_url)
    if response["status"] != 200:
        raise Exception(f"GET /color failed: {response['status']}")
    return json.loads(response["body"])


def get_hue(color_url: str) -> float:
    """Get current hue value"""
    return get_color(color_url)["hsv"]["h"]


def get_sat(color_url: str) -> float:
    """Get current saturation value"""
    return get_color(color_url)["hsv"]["s"]


def get_val(color_url: str) -> float:
    """Get current value (brightness)"""
    return get_color(color_url)["hsv"]["v"]


def get_selected_color(smoke_config: SmokeConfig, index: int) -> dict:
    return get_color(f"{smoke_config.color_url}?light={index}")


def get_raw_channels(color: dict) -> dict | list:
    if "raw" not in color:
        raise AssertionError(f"Color response has no raw state: {color}")
    raw = color["raw"]
    if "named" in raw:
        return raw["named"]
    if "indexed" in raw:
        return raw["indexed"]
    return raw


def post_json(base_url: str, endpoint: str, payload: dict) -> dict:
    return http_request(
        "POST",
        f"{base_url}/{endpoint}",
        body=json.dumps(payload, separators=(",", ":")).encode(),
        headers={"Content-Type": "application/json"},
    )


def rgbww_set(base_url: str, h: float, s: float, v: float) -> None:
    """Set solid color"""
    post_data = jsonTemplSolid.format(h=h, s=s, v=v)
    do_post(base_url, "color", post_data)


def set_hue_fade(base_url: str, hue: float, ramp: float, sat: float = 100, val: float = 100, queuePolicy: str = "back") -> float:
    """Start a hue fade, return start timestamp"""
    ts = time.time()
    print(f"Setting hue {hue} with ramp {ramp} s")
    post_data = jsonTempl.format(hue=hue, val=val, sat=sat, time=int(ramp * 1000), queue=queuePolicy, cmd="fade")
    do_post(base_url, "color", post_data)
    return ts


def set_channel_cmd(base_url: str, cmd: str, channels: str = '"h","s","v"') -> float:
    """Send channel command (pause, continue, etc)"""
    ts = time.time()
    print(f"Setting {cmd}")
    post_data = jsonTemplChannels.format(channels=channels.replace("'", '"'))
    do_post(base_url, cmd, post_data)
    return ts


def ws_recv_color_event(sock: socket.socket, timeout: float = 2.0) -> Optional[dict]:
    """Receive frames until a color_event is found, return the parsed JSON or None"""
    start_time = time.time()
    while time.time() - start_time < timeout:
        try:
            opcode, payload = ws_recv_frame(sock, timeout=0.5)
        except TimeoutError:
            continue
        if opcode is None:
            continue
        if opcode == 1:  # Text frame
            try:
                event = json.loads((payload or b"").decode("utf-8", errors="replace"))
                if event.get("method") == "color_event":
                    return event
            except (json.JSONDecodeError, ValueError):
                pass
        elif opcode == 9:  # Ping
            # Send pong response
            pong_payload = payload or b""
            sock.sendall(bytes([0x8A, len(pong_payload)]) + pong_payload)
    return None


def ws_recv_response(sock: socket.socket, request_id: str, timeout: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            opcode, payload = ws_recv_frame(sock, timeout=0.5)
        except TimeoutError:
            continue
        if opcode == 9:
            ping = payload or b""
            sock.sendall(bytes([0x8A, len(ping)]) + ping)
            continue
        if opcode != 1 or payload is None:
            continue
        response = json.loads(payload.decode("utf-8", errors="replace"))
        if str(response.get("id")) == request_id:
            return response
    raise AssertionError(f"No WebSocket response for request {request_id}")


@pytest.mark.parametrize("light_type,pins,meaningful_hsv,raw_channels,control_channel", LIGHT_PROFILES)
def test_each_light_type_command_suite(
    smoke_config: SmokeConfig,
    light_type: str,
    pins: list[int],
    meaningful_hsv: dict[str, str],
    raw_channels: list[str],
    control_channel: str,
) -> None:
    """HSVCT uses meaningful components and raw writes target the profile's supported channels."""
    if os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1":
        pytest.skip("creates virtual lights on the Host emulator only")
    index = 40
    _configure_lights(smoke_config, [_light(light_type, pins, index, True)])
    try:
        result = post_json(
            smoke_config.base_url,
            "color",
            {"light": index, "hsv": HSVCT_INPUT, "cmd": "solid"},
        )
        assert result["status"] == 200, f"{light_type} HSVCT command rejected: {result}"
        wait_for_fade_complete(0)
        for field in meaningful_hsv:
            expected = float(meaningful_hsv[field])
            assert_eventually_close(
                lambda field=field: float(get_selected_color(smoke_config, index)["hsv"][field]),
                expected,
                ASSERT_TOLERANCE,
                f"{light_type} expected HSVCT {field}={expected}",
            )

        for endpoint in ("stop", "skip", "pause", "continue", "blink", "toggle", "on", "off"):
            result = post_json(
                smoke_config.base_url,
                f"{endpoint}?light={index}",
                {"channels": [control_channel]},
            )
            assert result["status"] == 200, f"{light_type} {endpoint} command rejected: {result}"

        rgb_based = light_type in {"RGBWWCW", "RGBWW", "RGBCW", "RGB"}
        for value, channel in enumerate(raw_channels, start=420):
            raw_value = {"raw": {channel: str(value)}} if rgb_based else {
                "raw": [str(value) if index == value - 420 else None for index in range(len(raw_channels))]
            }
            result = post_json(
                smoke_config.base_url,
                f"color?light={index}",
                {**raw_value, "cmd": "solid"},
            )
            assert result["status"] == 200, f"{light_type} raw {channel} command rejected: {result}"
            wait_for_fade_complete(0)
            raw_response = get_selected_color(smoke_config, index)["raw"]
            assert isinstance(raw_response, dict if rgb_based else list), (
                f"{light_type} raw readback used the wrong wire shape: {raw_response}"
            )
            assert_eventually_close(
                lambda value=value, channel=channel: float(
                    get_raw_channels(get_selected_color(smoke_config, index))[channel if rgb_based else value - 420]
                ),
                float(value),
                ASSERT_TOLERANCE,
                f"{light_type} expected raw channel {channel}={value}",
            )

        if raw_channels:
            off_payload = {"raw": {raw_channels[0]: "0"}} if rgb_based else {
                "raw": ["0"] + [None] * (len(raw_channels) - 1)
            }
            result = post_json(
                smoke_config.base_url,
                f"color?light={index}",
                {**off_payload, "cmd": "solid"},
            )
            assert result["status"] == 200, f"{light_type} raw off command rejected: {result}"
            wait_for_fade_complete(0)
            assert_eventually_close(
                lambda: float(
                    get_raw_channels(get_selected_color(smoke_config, index))[raw_channels[0] if rgb_based else 0]
                ),
                0,
                ASSERT_TOLERANCE,
                f"{light_type} expected explicit zero to turn off its first channel",
            )
    finally:
        _configure_lights(smoke_config, [_light("RGBWW", [5, 4, 1, 0], 10, True)])


def test_mixed_light_http_and_websocket_selection(smoke_config: SmokeConfig) -> None:
    """HTTP and WebSocket selectors address the same configured light without cross-talk."""
    if os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1":
        pytest.skip("creates virtual lights on the Host emulator only")
    _configure_lights(
        smoke_config,
        [
            _light("WWCW", [0, 3], 23, False),
            _light("RGB", [5, 4, 1], 10, True),
        ],
    )
    try:
        default_result = post_json(
            smoke_config.base_url,
            "color",
            {"hsv": HSVCT_INPUT, "cmd": "solid"},
        )
        assert default_result["status"] == 200
        white_result = post_json(
            smoke_config.base_url,
            "color",
            {"light": 23, "hsv": {"h": "120", "s": "80", "v": "60", "ct": "3200"}, "cmd": "solid"},
        )
        assert white_result["status"] == 200
        wait_for_fade_complete(0)
        assert abs(float(get_color(smoke_config.color_url)["hsv"]["v"]) - 70) < ASSERT_TOLERANCE
        assert abs(float(get_selected_color(smoke_config, 23)["hsv"]["v"]) - 60) < ASSERT_TOLERANCE

        conflict = post_json(
            smoke_config.base_url,
            "color?light=23",
            {"light": 10, "raw": {"ww": "300"}},
        )
        assert conflict["status"] == 400
        assert http_request("GET", f"{smoke_config.color_url}?light=65535")["status"] == 400

        sock, _ = websocket_handshake(smoke_config, os.urandom(16))
        try:
            ws_send_text(sock, json.dumps({"jsonrpc": "2.0", "id": "23", "method": "color", "params": {"light": 23}}))
            response = ws_recv_response(sock, "23")
            assert "error" not in response, response
            assert abs(float(response["result"]["hsv"]["v"]) - 60) < ASSERT_TOLERANCE
            ws_send_text(
                sock,
                json.dumps(
                    {
                        "jsonrpc": "2.0",
                        "id": "24",
                        "method": "color",
                        "params": {"light": 23, "raw": ["480", None], "cmd": "solid"},
                    }
                ),
            )
            response = ws_recv_response(sock, "24")
            assert "error" not in response, response
        finally:
            sock.close()
        wait_for_fade_complete(0)
        assert float(get_raw_channels(get_selected_color(smoke_config, 23))[0]) == 480
        assert abs(float(get_color(smoke_config.color_url)["hsv"]["v"]) - 70) < ASSERT_TOLERANCE
    finally:
        _configure_lights(smoke_config, [_light("RGBWW", [5, 4, 1, 0], 10, True)])


@pytest.mark.parametrize(
    "light_type,pins,raw,channel",
    [
        ("RGB", [5, 4, 1], {"r": "440"}, "r"),
        ("WWCW", [0, 3], ["440", None], 0),
    ],
)
def test_raw_normalization_small_and_batched(smoke_config: SmokeConfig, light_type: str, pins: list[int], raw, channel) -> None:
    """Small and batched raw commands retain their channel values on HTTP and WebSocket."""
    if os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1":
        pytest.skip("creates virtual lights on the Host emulator only")
    index = 40
    _configure_lights(smoke_config, [_light(light_type, pins, index, True)])
    try:
        result = post_json(smoke_config.base_url, "color", {"light": index, "raw": raw, "cmd": "solid"})
        assert result["status"] == 200, result
        wait_for_fade_complete(0)
        assert_eventually_close(
            lambda: float(get_raw_channels(get_selected_color(smoke_config, index))[channel]),
            440,
            ASSERT_TOLERANCE,
            f"{light_type} small raw command",
        )

        batch = {"light": index, "cmds": [{"raw": raw, "cmd": "solid"} for _ in range(12)]}
        result = post_json(smoke_config.base_url, "color", batch)
        assert result["status"] == 200, result

        sock, _ = websocket_handshake(smoke_config, os.urandom(16))
        try:
            ws_send_text(sock, json.dumps({"jsonrpc": "2.0", "id": "41", "method": "color", "params": batch}))
            response = ws_recv_response(sock, "41")
            assert "error" not in response, response
        finally:
            sock.close()
        wait_for_fade_complete(0)
        assert_eventually_close(
            lambda: float(get_raw_channels(get_selected_color(smoke_config, index))[channel]),
            440,
            ASSERT_TOLERANCE,
            f"{light_type} batched raw command",
        )
    finally:
        _configure_lights(smoke_config, [_light("RGBWW", [5, 4, 1, 0], 10, True)])


def test_raw_normalization_rejects_oversized_batch(smoke_config: SmokeConfig) -> None:
    """Oversized raw batches fail explicitly without applying any command."""
    if os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1":
        pytest.skip("exercises normalization limits on the Host emulator only")
    initial = post_json(smoke_config.base_url, "color", {"raw": {"r": "440"}, "cmd": "solid"})
    assert initial["status"] == 200, initial
    wait_for_fade_complete(0)
    batch = {"cmds": [{"raw": {"r": "100"}, "cmd": "solid"} for _ in range(80)]}
    assert len(json.dumps(batch, separators=(",", ":"))) < 4096
    result = post_json(smoke_config.base_url, "color", batch)
    assert result["status"] == 400, result
    assert "normalization capacity" in result["body"], result
    oversized_raw = post_json(smoke_config.base_url, "color", {"raw": {"r": "1" * 1200}, "cmd": "solid"})
    assert oversized_raw["status"] == 400, oversized_raw
    assert "normalization capacity" in oversized_raw["body"], oversized_raw
    assert_eventually_close(
        lambda: float(get_raw_channels(get_color(smoke_config.color_url))["r"]),
        440,
        ASSERT_TOLERANCE,
        "oversized batch must not change the active light",
    )


# Pytest tests

def test_simple_fade(smoke_config: SmokeConfig) -> None:
    """Test a simple fade transition"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    new_hue = 120
    ramp = scaled(10)
    set_hue_fade(smoke_config.base_url, new_hue, ramp)
    wait_for_fade_complete(ramp)
    assert_eventually_close(
        lambda: get_hue(smoke_config.color_url),
        new_hue,
        ASSERT_TOLERANCE,
        f"Expected hue ~{new_hue}",
    )


def test_ramp_accuracy(smoke_config: SmokeConfig) -> None:
    """Repeat 60-second fades and verify accuracy"""
    ramp = scaled(RAMP_ACCURACY_RAMP_SECONDS)
    for i in range(1, RAMP_ACCURACY_ITERATIONS + 1):
        rgbww_set(smoke_config.base_url, 0, 100, 100)
        set_hue_fade(smoke_config.base_url, 120, ramp)
        wait_for_fade_complete(ramp)
        assert_eventually_close(
            lambda: get_hue(smoke_config.color_url),
            120,
            ASSERT_TOLERANCE,
            f"Iteration {i}: Expected hue ~120",
        )


def test_queue_back(smoke_config: SmokeConfig) -> None:
    """Test default queue (back) behavior"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = scaled(10)
    set_hue_fade(smoke_config.base_url, 120, ramp)
    set_hue_fade(smoke_config.base_url, 170, ramp)
    wait_for_fade_complete(2 * ramp)
    
    assert_eventually_close(
        lambda: get_hue(smoke_config.color_url),
        170,
        ASSERT_TOLERANCE,
        "Expected hue ~170",
    )


def test_queue_front_reset(smoke_config: SmokeConfig) -> None:
    """Test front_reset queue policy"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = scaled(12)
    delta = ASSERT_TOLERANCE
    set_hue_fade(smoke_config.base_url, 120, ramp)
    time.sleep(scaled(6))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 60, delta, "hue1: expected ~60")
    set_hue_fade(smoke_config.base_url, 30, ramp, queuePolicy="front_reset")
    time.sleep(scaled(6))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 45, delta, "hue2: expected ~45")
    time.sleep(scaled(6))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 30, delta, "hue3: expected ~30")
    time.sleep(scaled(6))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 75, delta, "hue4: expected ~75")


def test_queue_front(smoke_config: SmokeConfig) -> None:
    """Test front queue policy"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = scaled(12)
    delta = ASSERT_TOLERANCE
    set_hue_fade(smoke_config.base_url, 120, ramp)
    time.sleep(scaled(6))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 60, delta, "hue1: expected ~60")
    set_hue_fade(smoke_config.base_url, 30, ramp, queuePolicy="front")
    time.sleep(scaled(6))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 45, delta, "hue2: expected ~45")
    time.sleep(scaled(6))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 60, delta, "hue3: expected ~60")
    time.sleep(scaled(3))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 90, delta, "hue4: expected ~90")


def test_relative_plus(smoke_config: SmokeConfig) -> None:
    """Test relative hue increase"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = scaled(3)
    delta = ASSERT_TOLERANCE
    set_hue_fade(smoke_config.base_url, "+10", ramp)
    wait_for_fade_complete(ramp)
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 10, delta, "Expected hue ~10")


def test_relative_plus_circle_top(smoke_config: SmokeConfig) -> None:
    """Test relative hue increase wrapping around 360"""
    set_hue_fade(smoke_config.base_url, "350", 0)
    ramp = scaled(3)
    set_hue_fade(smoke_config.base_url, "+20", ramp)
    wait_for_fade_complete(ramp)
    delta = ASSERT_TOLERANCE
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 10, delta, "Expected hue ~10 (wrap)")


def test_relative_plus_multiple(smoke_config: SmokeConfig) -> None:
    """Test multiple relative hue increases"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = scaled(3)
    delta = ASSERT_TOLERANCE
    set_hue_fade(smoke_config.base_url, "+10", ramp)
    set_hue_fade(smoke_config.base_url, "+10", ramp)
    wait_for_fade_complete(ramp)
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 10, delta, "hue1: expected ~10")
    wait_for_fade_complete(ramp)
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 20, delta, "hue2: expected ~20")


def test_relative_minus(smoke_config: SmokeConfig) -> None:
    """Test relative hue decrease"""
    set_hue_fade(smoke_config.base_url, "100", 0)
    ramp = scaled(3)
    set_hue_fade(smoke_config.base_url, "-10", ramp)
    wait_for_fade_complete(ramp)
    delta = ASSERT_TOLERANCE
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 90, delta, "Expected hue ~90")


def test_relative_minus_circle_bottom(smoke_config: SmokeConfig) -> None:
    """Test relative hue decrease wrapping around 0"""
    set_hue_fade(smoke_config.base_url, "100", 0)
    ramp = scaled(3)
    set_hue_fade(smoke_config.base_url, "-150", ramp)
    wait_for_fade_complete(ramp)
    delta = ASSERT_TOLERANCE
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 310, delta, "Expected hue ~310 (wrap)")


def test_pause_all(smoke_config: SmokeConfig) -> None:
    """Test pause/continue of all channels"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    delta = ASSERT_TOLERANCE
    
    set_hue_fade(smoke_config.base_url, "100", val=50, sat=50, ramp=scaled(10))
    time.sleep(scaled(5))
    set_channel_cmd(smoke_config.base_url, "pause")
    time.sleep(scaled(5))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 50, delta, "hue1: expected ~50")
    assert_eventually_close(lambda: get_sat(smoke_config.color_url), 75, delta, "sat1: expected ~75")
    assert_eventually_close(lambda: get_val(smoke_config.color_url), 75, delta, "val1: expected ~75")
    set_channel_cmd(smoke_config.base_url, "continue")
    wait_for_fade_complete(scaled(5))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 100, delta, "hue2: expected ~100")
    assert_eventually_close(lambda: get_sat(smoke_config.color_url), 50, delta, "sat2: expected ~50")
    assert_eventually_close(lambda: get_val(smoke_config.color_url), 50, delta, "val2: expected ~50")


def test_pause_channel(smoke_config: SmokeConfig) -> None:
    """Test pause/continue of individual channel"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    delta = ASSERT_TOLERANCE
    
    set_hue_fade(smoke_config.base_url, "100", val=50, sat=50, ramp=scaled(10))
    time.sleep(scaled(5))
    set_channel_cmd(smoke_config.base_url, "pause", '"h"')
    time.sleep(scaled(5))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 50, delta, "hue1: expected ~50")
    assert_eventually_close(lambda: get_sat(smoke_config.color_url), 50, delta, "sat1: expected ~50")
    assert_eventually_close(lambda: get_val(smoke_config.color_url), 50, delta, "val1: expected ~50")
    set_channel_cmd(smoke_config.base_url, "continue", '"h"')
    wait_for_fade_complete(scaled(5))
    assert_eventually_close(lambda: get_hue(smoke_config.color_url), 100, delta, "hue2: expected ~100")
    assert_eventually_close(lambda: get_sat(smoke_config.color_url), 50, delta, "sat2: expected ~50")
    assert_eventually_close(lambda: get_val(smoke_config.color_url), 50, delta, "val2: expected ~50")


def test_websocket_color_event_on_set(smoke_config: SmokeConfig) -> None:
    """Verify websocket receives color_event when setting solid color"""
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        # Check handshake succeeded
        status_line = handshake_trace["response"]["status_line"]
        assert "101" in status_line, f"WebSocket handshake failed: {status_line}"
        
        # Set a color and wait for event
        rgbww_set(smoke_config.base_url, 45, 80, 90)
        
        # Receive color event
        event = ws_recv_color_event(sock, timeout=3.0)
        assert event is not None, "No color_event received from WebSocket"
        assert event.get("method") == "color_event", f"Expected color_event, got {event.get('method')}"
        
        # Verify params contain HSV data
        params = event.get("params", {})
        raw = params.get("raw", {})
        assert "h" in raw or "hsv" in params, f"No HSV data in event: {event}"
        
    finally:
        sock.close()


def test_websocket_color_event_on_fade(smoke_config: SmokeConfig) -> None:
    """Verify websocket receives color_event updates during fade"""
    sock, handshake_trace = websocket_handshake(smoke_config, os.urandom(16))
    try:
        status_line = handshake_trace["response"]["status_line"]
        assert "101" in status_line, f"WebSocket handshake failed: {status_line}"
        
        # Start a fade
        rgbww_set(smoke_config.base_url, 0, 100, 100)
        set_hue_fade(smoke_config.base_url, 180, scaled(5))  # 5 second fade
        
        # Receive multiple color events during fade
        events_received = []
        for _ in range(3):
            event = ws_recv_color_event(sock, timeout=2.0)
            if event:
                events_received.append(event)
        
        assert len(events_received) > 0, "No color_event received during fade"
        
        # Verify all events have the expected structure
        for event in events_received:
            assert event.get("method") == "color_event", f"Invalid event method: {event.get('method')}"
            assert "params" in event, f"No params in event: {event}"
            
    finally:
        sock.close()
