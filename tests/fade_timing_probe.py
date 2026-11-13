#!/usr/bin/env python3
"""Separate PWM duration from command/state/transport overhead on a diagnostic ESP32 build."""

import argparse
import asyncio
import csv
import io
import json
import pathlib
import statistics
import time
import urllib.request

from virtual_light_exerciser import ApiLog, Http, Light, WsClient, compare, unwrap_color


def parse_trace(text):
    if not text.startswith("# light=") or ",dropped=0\n" not in text.splitlines(keepends=True)[0]:
        raise RuntimeError("Missing or overflowed firmware trace")
    return [{key: int(value) for key, value in row.items()}
            for row in csv.DictReader(io.StringIO(text.split("\n", 1)[1]))]


def analyse(rows, frames, light, mode, target, started, scalar_key, client_port, acknowledged):
    states = [row for row in rows if row["event"] == 13]
    sent = {row["key"]: row for row in rows if row["event"] == 15 and row["value"] == client_port}
    received = [frame for frame in frames
                if frame["message"].get("method") == "light_state"
                and frame["message"].get("params", {}).get("light") == light.index]
    if len(states) != len(received) or any(row["id"] not in sent for row in states):
        raise RuntimeError("State/send/receive counts do not match; cannot correlate timing")
    found = next(((row, frame) for row, frame in zip(states, received)
                  if compare(light, mode, target, frame["message"]["params"]["state"])[0]), None)
    if found is None:
        raise RuntimeError("Target was not observed")
    state, frame = found
    expected = target[scalar_key]
    reported = frame["message"]["params"]["state"][mode]
    reported = reported[scalar_key]
    actual = state["detail"] if scalar_key == "ct" else state["value"]
    expected_wire = actual / 1023 * 100 if mode == "hsv" and scalar_key == "v" else actual
    if abs(reported - expected_wire) > 0.01:
        raise RuntimeError("Correlated controller/client state values differ")
    commands = [row for row in rows if row["event"] == 10]
    if len(commands) != 1:
        raise RuntimeError("Expected one command; another client may have changed the light")
    command = commands[0]
    show_done = next((row for row in rows if row["event"] == 12 and row["detail"] & 0x80000000), None)
    starts = [row for row in rows if row["event"] == 0]
    ends = [row for row in rows if row["event"] == 2]
    rendered = next(row for row in rows if row["event"] == 14 and row["key"] == state["id"])
    handoff = sent[state["id"]]
    host_ms = (frame["received_at"] - started) * 1000
    device_ms = (handoff["us"] - command["us"]) / 1000
    roundtrip_ms = (acknowledged - started) * 1000
    residual_ms = host_ms - device_ms
    result = {
        "light": light.index, "mode": mode, "target": expected,
        "host_observed_ms": host_ms,
        "command_to_send_ms": device_ms,
        "transport_and_host_residual_ms": residual_ms,
        "command_roundtrip_ms": roundtrip_ms,
        # Command processing is inside [started, acknowledged], giving clock-offset
        # bounds without assuming symmetric network delays.
        "handoff_to_receive_lower_ms": max(0, residual_ms - roundtrip_ms),
        "handoff_to_receive_upper_ms": residual_ms,
        "state_to_render_ms": (rendered["us"] - state["us"]) / 1000,
        "render_to_send_ms": (handoff["us"] - rendered["us"]) / 1000,
        "host_decode_ms": (frame["decoded_at"] - frame["received_at"]) * 1000,
        "observed_target": reported,
    }
    if starts and ends:
        begin = starts[0]
        end = ends[-1]
        pwm_ms = (end["us"] - begin["us"]) / 1000
        result.update(
            hardware_segments=len(starts), pwm_duration_ms=pwm_ms,
            command_to_pwm_start_ms=(begin["us"] - command["us"]) / 1000,
            pwm_done_to_state_ms=(state["us"] - end["us"]) / 1000,
            measured_minus_pwm_ms=host_ms - pwm_ms,
        )
        if show_done is not None:
            result["pwm_done_to_show_done_ms"] = (show_done["us"] - end["us"]) / 1000
    elif show_done is not None:
        # Application completion, not a physical PWM duration measurement.
        first_show = next(row for row in rows if row["event"] == 12 and row["us"] >= command["us"])
        result["command_to_software_done_ms"] = (show_done["us"] - command["us"]) / 1000
        result["software_show_span_ms"] = (show_done["us"] - first_show["us"]) / 1000
        result["software_done_to_state_ms"] = (state["us"] - show_done["us"]) / 1000
    else:
        raise RuntimeError("Neither hardware nor software completion was captured")
    return result


async def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    log = ApiLog(args.output / "api.jsonl")
    http = Http(f"http://{args.host}", args.password, 10, log)
    ws = WsClient(f"ws://{args.host}/ws", args.password, False)
    try:
        info = await http.get("/info")
        configuration = (await http.get("/config"))["hardware"]["configuration"]
        selected = next(entry for entry in configuration["lights"] if entry["index"] == args.light)
        if selected["type"] not in ("SINGLE", "WW", "WWCW"):
            raise RuntimeError("Probe currently correlates warm-white output for SINGLE, WW and WWCW lights only")
        light = Light(index=selected["index"], id=selected["id"], name=selected["name"],
                      type=selected["type"], is_default=selected["is_default"], ct_ww=2700, ct_cw=6000)
        original = unwrap_color(await http.get(f"/color?light={args.light}"))
        await ws.connect()
        if not await ws.subscribe_lights():
            raise RuntimeError("Diagnostic probe requires light_state subscriptions")
        results = []
        plans = [
            ("hsv", {"v": 5}, {"v": 95}, "v"),
            ("raw", [0] * len(selected["pins"]), [1000] + [0] * (len(selected["pins"]) - 1), 0),
        ]
        if selected["type"] == "WWCW":
            plans.append(("hsv", {"v": 60, "ct": 2700}, {"v": 60, "ct": 6000}, "ct"))
        def body(mode, value, duration=0):
            return {"cmd": "fade" if duration else "solid", mode: value, "t": duration,
                    "q": "single", "light": args.light, "light_id": light.id}
        try:
            for repeat in range(args.repeats):
                for mode, start, target, key in plans:
                    await http.post("/color", body(mode, start))
                    await asyncio.sleep(0.7)
                    ws.timing_messages = []
                    await http.post(f"/debug/fade-timing?light={args.light}", {})
                    started = time.monotonic()
                    await http.post("/color", body(mode, target, args.fade_ms))
                    acknowledged = time.monotonic()
                    await ws.wait_for_event(
                        lambda event: event.light == args.light and event.t >= started
                        and compare(light, mode, target, event.state)[0],
                        args.fade_ms / 1000 + 12,
                    )
                    await asyncio.sleep(1)
                    request = urllib.request.Request(http.base_url + "/debug/fade-timing", headers=http.headers)
                    def download():
                        with urllib.request.urlopen(request, timeout=10) as response:
                            return response.read().decode()
                    trace = await asyncio.to_thread(download)
                    rows = parse_trace(trace)
                    state_count = sum(row["event"] == 13 for row in rows)
                    deadline = time.monotonic() + 3
                    while sum(frame["message"].get("method") == "light_state"
                              and frame["message"].get("params", {}).get("light") == args.light
                              for frame in ws.timing_messages) < state_count and time.monotonic() < deadline:
                        await asyncio.sleep(0.01)
                    frames = ws.timing_messages
                    ws.timing_messages = None
                    client_port = ws.ws.local_address[1]
                    name = f"light-{args.light}-{repeat}-{mode}-{key}"
                    (args.output / f"{name}.csv").write_text(trace)
                    (args.output / f"{name}.json").write_text(json.dumps({
                        "started": started, "acknowledged": acknowledged, "frames": frames,
                        "firmware": info["app"], "client_port": client_port,
                    }, indent=2))
                    result = analyse(rows, frames, light, mode, target, started, key, client_port, acknowledged)
                    results.append(result)
                    print(json.dumps(result), flush=True)
        finally:
            restore_mode = "hsv" if "hsv" in original else "raw"
            await http.post("/color", body(restore_mode, original[restore_mode]))
        (args.output / f"summary-{args.light}.json").write_text(json.dumps(results, indent=2))
        for key in ("host_observed_ms", "pwm_duration_ms", "measured_minus_pwm_ms",
                    "transport_and_host_residual_ms", "host_decode_ms"):
            values = [result[key] for result in results if key in result]
            if values:
                print(f"{key}: min={min(values):.3f}, median={statistics.median(values):.3f}, max={max(values):.3f}")
    finally:
        await ws.close()
        log.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--light", type=int, required=True)
    parser.add_argument("--password")
    parser.add_argument("--fade-ms", type=int, default=3000)
    parser.add_argument("--repeats", type=int, default=4)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    options = parser.parse_args()
    if options.fade_ms <= 0 or options.repeats <= 0:
        parser.error("--fade-ms and --repeats must be positive")
    asyncio.run(run(options))
