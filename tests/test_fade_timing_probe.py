import pytest

from fade_timing_probe import analyse, parse_trace
from virtual_light_exerciser import Light


def entry(event, us, key=4, value=0, detail=0, identifier=1):
    return {"id": identifier, "us": us, "event": event, "key": key, "value": value, "detail": detail}


def capture():
    rows = [
        entry(10, 1000000), entry(0, 1010000), entry(2, 4010000),
        entry(12, 4020000, value=972, detail=0x80000000),
        entry(13, 4100000, value=972, identifier=5),
        entry(14, 4110000, key=5),
        entry(15, 4111000, key=5, value=1234),
        entry(15, 4150000, key=5, value=5678),
    ]
    frames = [{"received_at": 13.2, "decoded_at": 13.20002,
               "message": {"method": "light_state", "params": {"light": 4, "state": {"hsv": {"v": 972 / 1023 * 100}}}}}]
    light = Light(4, "id", "RED", "SINGLE", False)
    return rows, frames, light


def test_separates_hardware_duration_and_measurement_overhead():
    rows, frames, light = capture()
    result = analyse(rows, frames, light, "hsv", {"v": 95}, 10, "v", 1234, 10.04)
    assert result["pwm_duration_ms"] == 3000
    assert result["measured_minus_pwm_ms"] == pytest.approx(200)
    assert result["transport_and_host_residual_ms"] == pytest.approx(89)
    assert result["state_to_render_ms"] == 10
    assert result["render_to_send_ms"] == 1
    assert result["host_decode_ms"] == pytest.approx(0.02)
    assert result["handoff_to_receive_lower_ms"] == pytest.approx(49)
    assert result["handoff_to_receive_upper_ms"] == pytest.approx(89)


def test_observation_can_precede_physical_completion():
    rows, frames, light = capture()
    rows[2]["us"] = 4300000
    result = analyse(rows, frames, light, "hsv", {"v": 95}, 10, "v", 1234, 10.04)
    assert result["pwm_done_to_state_ms"] == -200
    assert result["measured_minus_pwm_ms"] < 0


def test_rejects_uncorrelated_or_overflowed_traces():
    rows, frames, light = capture()
    with pytest.raises(RuntimeError, match="counts"):
        analyse(rows, [], light, "hsv", {"v": 95}, 10, "v", 1234, 10.04)
    with pytest.raises(RuntimeError, match="overflowed"):
        parse_trace("# light=4,dropped=1\nid,us,event,key,value,detail\n")
    assert parse_trace("# light=4,dropped=0\nid,us,event,key,value,detail\n1,100,10,4,0,0\n")[0]["us"] == 100
