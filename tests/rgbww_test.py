"""
Created on 02.04.2017

@author: Robin

Converted to pytest format integrated with host_smoke_api_test.py harness
"""
import json
import time
from urllib import error, request

import pytest

# Import helpers from host_smoke_api_test
from host_smoke_api_test import SmokeConfig, http_request, smoke_config  # noqa: F401

# JSON templates for color commands
jsonTempl = """{{
  "q":"{queue}",
  "hsv":{{"ct":2700,"v":"{val}","h":"{hue}","s":"{sat}"}},
  "d":1,
  "t":{time},
  "cmd":"{cmd}"
}}
"""

jsonTemplChannels = """{{channels:[{channels}]}}"""

jsonTemplSolid = """{{
  "hsv":{{"ct":2700,"v":"{v}","h":"{h}","s":"{s}"}},
  "d":1,
  "cmd":"solid"
}}
"""


def do_post(base_url: str, endpoint: str, post_data: str) -> None:
    """Post data to endpoint, raise on error"""
    response = http_request("POST", f"{base_url}/{endpoint}", body=post_data.encode())
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


def rgbww_set(base_url: str, h: float, s: float, v: float) -> None:
    """Set solid color"""
    post_data = jsonTemplSolid.format(h=h, s=s, v=v)
    do_post(base_url, "color", post_data)


def set_hue_fade(base_url: str, hue: float, ramp: float, sat: float = 100, val: float = 100, queuePolicy: str = "back") -> float:
    """Start a hue fade, return start timestamp"""
    ts = time.time()
    print(f"Setting hue {hue} with ramp {ramp} s")
    post_data = jsonTempl.format(hue=hue, val=val, sat=sat, time=int(ramp * 1000), queue=queuePolicy, cmd="fade")
    response = http_request("POST", f"{base_url}/color", body=post_data.encode())
    if response["status"] != 200:
        raise Exception(f"set_hue_fade failed: {response['body']}")
    return ts


def set_channel_cmd(base_url: str, cmd: str, channels: str = "'h','s','v'") -> float:
    """Send channel command (pause, continue, etc)"""
    ts = time.time()
    print(f"Setting {cmd}")
    post_data = jsonTemplChannels.format(channels=channels)
    response = http_request("POST", f"{base_url}/{cmd}", body=post_data.encode())
    if response["status"] != 200:
        raise Exception(f"set_channel_cmd {cmd} failed: {response['status']}")
    return ts


# Pytest tests

def test_simple_fade(smoke_config: SmokeConfig) -> None:
    """Test a simple fade transition"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    new_hue = 120
    ramp = 10
    set_hue_fade(smoke_config.base_url, new_hue, ramp)
    time.sleep(ramp)
    cur_hue = get_hue(smoke_config.color_url)
    
    assert abs(cur_hue - new_hue) < 0.5, f"Expected hue ~{new_hue}, got {cur_hue}"


def test_ramp_accuracy(smoke_config: SmokeConfig) -> None:
    """Repeat 60-second fades and verify accuracy"""
    ramp = 60
    for i in range(1, 10):
        rgbww_set(smoke_config.base_url, 0, 100, 100)
        set_hue_fade(smoke_config.base_url, 120, ramp)
        time.sleep(ramp)
        cur_hue = get_hue(smoke_config.color_url)
        assert abs(cur_hue - 120) < 0.5, f"Iteration {i}: Expected hue ~120, got {cur_hue}"


def test_queue_back(smoke_config: SmokeConfig) -> None:
    """Test default queue (back) behavior"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = 10
    set_hue_fade(smoke_config.base_url, 120, ramp)
    set_hue_fade(smoke_config.base_url, 170, ramp)
    time.sleep(2 * ramp)
    
    assert abs(get_hue(smoke_config.color_url) - 170) < 0.5


def test_queue_front_reset(smoke_config: SmokeConfig) -> None:
    """Test front_reset queue policy"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = 12
    delta = 0.8
    set_hue_fade(smoke_config.base_url, 120, ramp)
    time.sleep(6)
    hue1 = get_hue(smoke_config.color_url)  # ~60
    set_hue_fade(smoke_config.base_url, 30, 12, queuePolicy="front_reset")
    time.sleep(6)
    hue2 = get_hue(smoke_config.color_url)  # ~45
    time.sleep(6)
    hue3 = get_hue(smoke_config.color_url)  # ~30
    time.sleep(6)
    hue4 = get_hue(smoke_config.color_url)  # ~75

    assert abs(hue1 - 60) < delta, f"hue1: expected ~60, got {hue1}"
    assert abs(hue2 - 45) < delta, f"hue2: expected ~45, got {hue2}"
    assert abs(hue3 - 30) < delta, f"hue3: expected ~30, got {hue3}"
    assert abs(hue4 - 75) < delta, f"hue4: expected ~75, got {hue4}"


def test_queue_front(smoke_config: SmokeConfig) -> None:
    """Test front queue policy"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = 12
    delta = 1.0
    set_hue_fade(smoke_config.base_url, 120, ramp)
    time.sleep(6)
    hue1 = get_hue(smoke_config.color_url)  # ~60
    set_hue_fade(smoke_config.base_url, 30, 12, queuePolicy="front")
    time.sleep(6)
    hue2 = get_hue(smoke_config.color_url)  # ~45
    time.sleep(6)
    hue3 = get_hue(smoke_config.color_url)  # ~60
    time.sleep(3)
    hue4 = get_hue(smoke_config.color_url)  # ~60

    assert abs(hue1 - 60) < delta, f"hue1: expected ~60, got {hue1}"
    assert abs(hue2 - 45) < delta, f"hue2: expected ~45, got {hue2}"
    assert abs(hue3 - 60) < delta, f"hue3: expected ~60, got {hue3}"
    assert abs(hue4 - 90) < delta, f"hue4: expected ~90, got {hue4}"


def test_relative_plus(smoke_config: SmokeConfig) -> None:
    """Test relative hue increase"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = 3
    delta = 0.8
    set_hue_fade(smoke_config.base_url, "+10", ramp)
    time.sleep(3)
    hue1 = get_hue(smoke_config.color_url)

    assert abs(hue1 - 10) < delta, f"Expected hue ~10, got {hue1}"


def test_relative_plus_circle_top(smoke_config: SmokeConfig) -> None:
    """Test relative hue increase wrapping around 360"""
    set_hue_fade(smoke_config.base_url, "350", 0)
    ramp = 3
    set_hue_fade(smoke_config.base_url, "+20", ramp)
    time.sleep(3)
    hue1 = get_hue(smoke_config.color_url)

    delta = 0.8
    assert abs(hue1 - 10) < delta, f"Expected hue ~10 (wrap), got {hue1}"


def test_relative_plus_multiple(smoke_config: SmokeConfig) -> None:
    """Test multiple relative hue increases"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    ramp = 3
    delta = 0.8
    set_hue_fade(smoke_config.base_url, "+10", ramp)
    set_hue_fade(smoke_config.base_url, "+10", ramp)
    time.sleep(3)
    hue1 = get_hue(smoke_config.color_url)
    time.sleep(3)
    hue2 = get_hue(smoke_config.color_url)

    assert abs(hue1 - 10) < delta, f"hue1: expected ~10, got {hue1}"
    assert abs(hue2 - 20) < delta, f"hue2: expected ~20, got {hue2}"


def test_relative_minus(smoke_config: SmokeConfig) -> None:
    """Test relative hue decrease"""
    set_hue_fade(smoke_config.base_url, "100", 0)
    ramp = 3
    set_hue_fade(smoke_config.base_url, "-10", ramp)
    time.sleep(ramp)
    hue1 = get_hue(smoke_config.color_url)

    delta = 0.8
    assert abs(hue1 - 90) < delta, f"Expected hue ~90, got {hue1}"


def test_relative_minus_circle_bottom(smoke_config: SmokeConfig) -> None:
    """Test relative hue decrease wrapping around 0"""
    set_hue_fade(smoke_config.base_url, "100", 0)
    ramp = 3
    set_hue_fade(smoke_config.base_url, "-150", ramp)
    time.sleep(3)
    hue1 = get_hue(smoke_config.color_url)

    delta = 0.8
    assert abs(hue1 - 310) < delta, f"Expected hue ~310 (wrap), got {hue1}"


def test_pause_all(smoke_config: SmokeConfig) -> None:
    """Test pause/continue of all channels"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    set_hue_fade(smoke_config.base_url, "100", val=50, sat=50, ramp=10)
    time.sleep(5)
    set_channel_cmd(smoke_config.base_url, "pause")
    time.sleep(5)
    hue1 = get_hue(smoke_config.color_url)
    sat1 = get_sat(smoke_config.color_url)
    val1 = get_val(smoke_config.color_url)
    set_channel_cmd(smoke_config.base_url, "continue")
    time.sleep(5)
    hue2 = get_hue(smoke_config.color_url)
    sat2 = get_sat(smoke_config.color_url)
    val2 = get_val(smoke_config.color_url)
    
    delta = 0.8
    assert abs(hue1 - 50) < delta, f"hue1: expected ~50, got {hue1}"
    assert abs(sat1 - 75) < delta, f"sat1: expected ~75, got {sat1}"
    assert abs(val1 - 75) < delta, f"val1: expected ~75, got {val1}"
    assert abs(hue2 - 100) < delta, f"hue2: expected ~100, got {hue2}"
    assert abs(sat2 - 50) < delta, f"sat2: expected ~50, got {sat2}"
    assert abs(val2 - 50) < delta, f"val2: expected ~50, got {val2}"


def test_pause_channel(smoke_config: SmokeConfig) -> None:
    """Test pause/continue of individual channel"""
    rgbww_set(smoke_config.base_url, 0, 100, 100)
    
    set_hue_fade(smoke_config.base_url, "100", val=50, sat=50, ramp=10)
    time.sleep(5)
    set_channel_cmd(smoke_config.base_url, "pause", "'h'")
    time.sleep(5)
    hue1 = get_hue(smoke_config.color_url)
    sat1 = get_sat(smoke_config.color_url)
    val1 = get_val(smoke_config.color_url)
    set_channel_cmd(smoke_config.base_url, "continue", "'h'")
    time.sleep(5)
    hue2 = get_hue(smoke_config.color_url)
    sat2 = get_sat(smoke_config.color_url)
    val2 = get_val(smoke_config.color_url)
    
    delta = 0.8
    assert abs(hue1 - 50) < delta, f"hue1: expected ~50, got {hue1}"
    assert abs(sat1 - 50) < delta, f"sat1: expected ~50, got {sat1}"
    assert abs(val1 - 50) < delta, f"val1: expected ~50, got {val1}"
    assert abs(hue2 - 100) < delta, f"hue2: expected ~100, got {hue2}"
    assert abs(sat2 - 50) < delta, f"sat2: expected ~50, got {sat2}"
    assert abs(val2 - 50) < delta, f"val2: expected ~50, got {val2}"