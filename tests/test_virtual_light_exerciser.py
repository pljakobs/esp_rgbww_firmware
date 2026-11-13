import asyncio
import itertools
import time

import pytest

from virtual_light_exerciser import (
    Event, Exerciser, Light, WsClient, concurrent_plan, parse_args,
)


def lights():
    return [
        Light(3, "green", "GREEN", "SINGLE", False),
        Light(4, "red", "RED", "SINGLE", False),
        Light(6, "pink", "PINK", "SINGLE", False),
        Light(7, "white", "Tunable", "WWCW", True),
    ]


def test_concurrent_plan_covers_every_subset_and_both_mixed_assignments():
    available = lights()
    cases = list(concurrent_plan(available))
    expected = {
        tuple(light.index for light in subset)
        for size in range(2, 5)
        for subset in itertools.combinations(available, size)
    }
    assert {tuple(light.index for light, _ in steps) for _, steps in cases} == expected
    for subset in expected:
        workloads = [steps for _, steps in cases if tuple(light.index for light, _ in steps) == subset]
        modes = {tuple(step[1] for _, step in steps) for steps in workloads}
        assert ("raw",) * len(subset) in modes
        assert ("hsv",) * len(subset) in modes
        assert tuple("raw" if i % 2 == 0 else "hsv" for i in range(len(subset))) in modes
        assert tuple("hsv" if i % 2 == 0 else "raw" for i in range(len(subset))) in modes
        signatures = [tuple(step[0] for _, step in steps) for steps in workloads]
        assert len(signatures) == len(set(signatures))
    assert len(cases) == 51
    assert any(step[4] == "ct" for _, assignments in cases for _, step in assignments)
    assert list(concurrent_plan(available[:1])) == []


@pytest.mark.parametrize("kind", ["RGB", "RGBWW", "RGBCW", "RGBWWCW", "WWCW", "WW", "CW", "SINGLE"])
def test_concurrent_plan_uses_supported_light_modes(kind):
    pair = [Light(i, str(i), kind, kind, False) for i in range(2)]
    cases = list(concurrent_plan(pair))
    assert cases
    for _, assignments in cases:
        for light, (_, mode, start, target, key) in assignments:
            assert start[key] != target[key]
            assert mode in ("raw", "hsv")
            if mode == "raw":
                assert isinstance(start, dict if light.has_rgb else list)
    if pair[0].has_rgb and not pair[0].has_ct:
        assert any(step[4] == "h" for _, assignments in cases for _, step in assignments)


@pytest.mark.parametrize("options", [
    ["--concurrent-only", "--skip-fades"],
    ["--concurrent-only", "--permissive"],
    ["--concurrent-only", "--soak-hours", "1"],
    ["--concurrent-only", "--skip-concurrent"],
    ["--fade-ms", "0"],
])
def test_invalid_concurrent_options(options):
    with pytest.raises(SystemExit):
        parse_args(["localhost", *options])


class Log:
    def __init__(self):
        self.records = []

    def write(self, record):
        self.records.append(record)


def test_concurrent_runner_dispatches_before_waiting_and_restores(monkeypatch, tmp_path):
    pair = lights()[:2]
    label, assignments = next(iter(concurrent_plan(pair)))
    monkeypatch.setattr("virtual_light_exerciser.concurrent_plan", lambda _: [(label, assignments)])

    async def exercise():
        args = parse_args(["localhost", "--no-reboot", "--dwell", "0", "--api-log", str(tmp_path / "api.jsonl")])
        runner = Exerciser(args)
        runner.api_log.close()
        runner.api_log = Log()
        runner.ws = WsClient("", None, False)
        states = {light.index: {"hsv": {"v": 0}} for light in pair}
        pending = []
        batches = []

        async def send(body):
            if "cmds" in body:
                batches.append(body["cmds"])
            for command in body.get("cmds", [body]):
                light = next(light for light in pair if light.index == command["light"])
                mode = "hsv" if "hsv" in command else "raw"
                if command["cmd"] == "fade":
                    pending.append(command)
                    since = time.monotonic()
                    for fraction in (0.1, 0.3, 0.5, 0.7, 0.9, 1):
                        value = [int(command[mode][0] * fraction)]
                        runner.ws.add_event(Event(since + 3.05 * fraction, light.index, light.id, {mode: value}))
                else:
                    runner.ws.add_event(Event(time.monotonic(), light.index, light.id, {mode: command[mode]}))
                states[light.index] = {mode: command[mode]}

        async def read_color(light):
            return states[light.index]

        async def wait_for_event(predicate, timeout):
            if pending:
                assert len(pending) == 2
            return next((e for e in runner.ws.events if predicate(e)), None)

        runner.send = send
        runner.read_color = read_color
        runner.ws.wait_for_event = wait_for_event
        await runner.run_concurrent(pair)
        assert len(pending) == 2
        assert len(batches) == 2
        assert all({command["light"] for command in batch} == {3, 4} for batch in batches)
        assert runner.results.failed == 0
        assert states == {light.index: {"hsv": {"v": 0}} for light in pair}
        assert len(runner.api_log.records) == 2
        assert {record["light"] for record in runner.api_log.records} == {3, 4}
        assert all(record["target_observed"] is not None for record in runner.api_log.records)

    asyncio.run(exercise())


def test_concurrent_runner_restores_after_send_failure(monkeypatch, tmp_path):
    pair = lights()[:2]
    label, assignments = next(iter(concurrent_plan(pair)))
    monkeypatch.setattr("virtual_light_exerciser.concurrent_plan", lambda _: [(label, assignments)])

    async def exercise():
        runner = Exerciser(parse_args(["localhost", "--dwell", "0", "--api-log", str(tmp_path / "api.jsonl")]))
        restored = []

        async def read_color(light):
            return {"hsv": {"v": light.index}}

        async def send(body):
            if any(command["cmd"] == "fade" for command in body["cmds"]):
                raise RuntimeError("test rejection")

        async def wait_for_event(predicate, timeout):
            return Event(time.monotonic(), 0, "", {})

        async def restore(light, original):
            restored.append((light.index, original))

        runner.read_color = read_color
        runner.send = send
        runner.ws.wait_for_event = wait_for_event
        runner.restore = restore
        try:
            await runner.run_concurrent(pair)
        finally:
            runner.api_log.close()
        assert runner.results.failed == 1
        assert restored == [(light.index, {"hsv": {"v": light.index}}) for light in pair]

    asyncio.run(exercise())


@pytest.mark.parametrize("options,individual,concurrent", [
    ([], True, True),
    (["--concurrent-only"], False, True),
    (["--skip-concurrent"], True, False),
    (["--skip-fades"], True, False),
    (["--light", "3", "--light", "7", "--concurrent-only"], False, True),
])
def test_run_routes_individual_and_concurrent_phases(tmp_path, options, individual, concurrent):
    async def exercise():
        runner = Exerciser(parse_args([
            "localhost", "--no-reboot", "--api-log", str(tmp_path / "api.jsonl"), *options,
        ]))
        available = lights()
        calls = []

        async def discover():
            return available

        async def read_color(light):
            return {"hsv": {"v": 0}}

        async def connected():
            return True

        async def record_solid(light):
            calls.append(("individual", light.index))

        async def record_fades(light):
            calls.append(("fade", light.index))

        async def record_concurrent(selected):
            calls.append(("concurrent", tuple(light.index for light in selected)))

        async def restore(light, original):
            calls.append(("restore", light.index))

        runner.discover = discover
        runner.read_color = read_color
        runner.ws.connect = connected
        runner.ws.subscribe_lights = connected
        runner.ws.close = connected
        runner.run_solid = record_solid
        runner.run_fades = record_fades
        runner.run_concurrent = record_concurrent
        runner.restore = restore
        try:
            assert await runner.run() == 0
        finally:
            runner.api_log.close()
        assert any(kind == "individual" for kind, _ in calls) == individual
        assert any(kind == "concurrent" for kind, _ in calls) == concurrent
        if concurrent:
            selected = (3, 7) if "--light" in options else (3, 4, 6, 7)
            assert ("concurrent", selected) in calls
        if "--skip-fades" in options:
            assert not any(kind == "fade" for kind, _ in calls)

    asyncio.run(exercise())


@pytest.mark.parametrize("via", ["http", "ws"])
def test_send_routes_single_and_batch_commands(tmp_path, via):
    async def exercise():
        runner = Exerciser(parse_args([
            "localhost", "--via", via, "--api-log", str(tmp_path / "api.jsonl"),
        ]))
        calls = []

        async def post(path, body):
            calls.append((path, body))

        async def call(method, body):
            calls.append((method, body))
            return {"result": True}

        runner.http.post = post
        runner.ws.call_authenticated = call
        single = runner.command(lights()[0], "raw", [100])
        batch = {"cmds": [single, runner.command(lights()[1], "raw", [200])]}
        try:
            await runner.send(single)
            await runner.send(batch)
        finally:
            runner.api_log.close()
        if via == "http":
            assert calls == [("/color?light=3", single), ("/color", batch)]
        else:
            assert calls == [("color", single), ("color", batch)]

    asyncio.run(exercise())


def test_restore_failure_marks_run_failed(tmp_path):
    async def exercise():
        runner = Exerciser(parse_args(["localhost", "--api-log", str(tmp_path / "api.jsonl")]))

        async def send(body):
            raise OSError("controller unavailable")

        runner.send = send
        try:
            await runner.restore(lights()[0], {"hsv": {"v": 0}})
        finally:
            runner.api_log.close()
        assert runner.results.failed == 1
        assert "controller unavailable" in runner.results.failures[0]

    asyncio.run(exercise())
