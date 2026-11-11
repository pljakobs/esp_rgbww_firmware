#!/usr/bin/env python3
"""Exercise virtual lights or run a low-intensity queued-fade soak.

By default, first requests a reboot and waits for an offline/online transition.
Use ``--no-reboot`` to exercise an already-running session instead.
``--permissive`` only sends exercise commands over HTTP: no reboot, WebSocket
validation or log-service dependency. Errors are reported and later steps continue.
For each light from ``/config`` (``hardware.configuration.lights``) the script

1. sends type-appropriate ``solid`` HSV/CT and RAW commands,
2. verifies the result via ``GET /color?light=N`` and the ``light_state``
   WebSocket notifications of the ``lights`` channel,
3. runs timed ``fade`` commands and analyses the ``light_state`` stream
   (number of intermediate updates, monotonic progress, duration, end value),
4. restores the original color of each light.

For an extended low-intensity run, ``--soak-hours`` uploads a five-minute
sequence of fast and slow fades per light, including individual and combined
channels. API responses and newly received controller syslog are timestamped
in the JSONL file selected by ``--api-log``.

Requires Python >= 3.10 and the ``websockets`` package (``pip install websockets``).

Example::

    tests/virtual_light_exerciser.py 192.168.1.50 --fade-ms 3000
    tests/virtual_light_exerciser.py lightinator.local --password secret --light 1 -v
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import contextlib
import datetime
import hashlib
import itertools
import json
import pathlib
import re
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any

try:
    import websockets
except ImportError:  # pragma: no cover - environment guard
    sys.exit("This script needs the 'websockets' package: pip install websockets")


RGB_TYPES = {"RGB", "RGBWW", "RGBCW", "RGBWWCW"}
WHITE_TYPES = {"RGBWW", "RGBCW", "RGBWWCW", "WWCW"}
# Channel names of the named RAW object for RGB-family lights.
NAMED_CHANNELS = {
    "RGB": ["r", "g", "b"],
    "RGBWW": ["r", "g", "b", "ww"],
    "RGBCW": ["r", "g", "b", "cw"],
    "RGBWWCW": ["r", "g", "b", "ww", "cw"],
}
# Pin-ordered RAW array length for the remaining types.
INDEXED_LENGTH = {"WWCW": 2, "WW": 1, "CW": 1, "SINGLE": 1}

TOL = {"h": 2.0, "s": 2.0, "v": 2.0, "ct": 60, "raw": 12}


# --------------------------------------------------------------------------- output


class Colors:
    enabled = sys.stdout.isatty()

    @classmethod
    def wrap(cls, code: str, text: str) -> str:
        return f"\033[{code}m{text}\033[0m" if cls.enabled else text


def ok(text: str) -> str:
    return Colors.wrap("32", text)


def bad(text: str) -> str:
    return Colors.wrap("31", text)


def dim(text: str) -> str:
    return Colors.wrap("2", text)


def head(text: str) -> str:
    return Colors.wrap("1;36", text)


# --------------------------------------------------------------------------- model


@dataclass
class Light:
    index: int
    id: str
    name: str
    type: str
    is_default: bool
    ct_ww: int = 2700
    ct_cw: int = 6000

    @property
    def has_rgb(self) -> bool:
        return self.type in RGB_TYPES

    @property
    def has_ct(self) -> bool:
        return self.type in WHITE_TYPES

    def label(self) -> str:
        default = ", default" if self.is_default else ""
        return f"light {self.index} '{self.name}' [{self.type}{default}] id={self.id}"


@dataclass
class Event:
    t: float
    light: int
    light_id: str
    state: dict[str, Any]
    source: str = "light_state"


@dataclass
class Results:
    passed: int = 0
    failed: int = 0
    failures: list[str] = field(default_factory=list)

    def check(self, cond: bool, what: str, detail: str = "") -> bool:
        if cond:
            self.passed += 1
            print(f"    {ok('PASS')} {what} {dim(detail)}")
        else:
            self.failed += 1
            self.failures.append(f"{what}: {detail}")
            print(f"    {bad('FAIL')} {what} {detail}")
        return cond


# --------------------------------------------------------------------------- HTTP


class Http:
    def __init__(self, base_url: str, password: str | None, timeout: float, api_log):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.api_log = api_log
        self.headers = {"Content-Type": "application/json"}
        if password:
            token = base64.b64encode(f"admin:{password}".encode()).decode()
            self.headers["Authorization"] = f"Basic {token}"

    def _request(self, method: str, path: str, body: Any = None) -> Any:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base_url + path, data=data, method=method, headers=self.headers)
        event = {
            "kind": "api",
            "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "method": method,
            "url": self.base_url + path,
            "request": body,
        }
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read()
                event["status"] = resp.status
            result = json.loads(raw) if raw else None
            event["response"] = result
            self.api_log.write(event)
            return result
        except urllib.error.HTTPError as err:
            detail = err.read().decode(errors="replace")
            event["status"] = err.code
            event["response"] = detail
            self.api_log.write(event)
            raise RuntimeError(f"{method} {path} -> HTTP {err.code}: {detail}") from None
        except Exception as err:
            event["error"] = str(err)
            self.api_log.write(event)
            raise

    async def get(self, path: str) -> Any:
        return await asyncio.to_thread(self._request, "GET", path)

    async def post(self, path: str, body: Any) -> Any:
        return await asyncio.to_thread(self._request, "POST", path, body)


class ApiLog:
    """Append timestamped API and correlated syslog records to one JSONL file."""

    _secret_key = re.compile(r"password|passwd|secret|token|credential|authorization|ssid", re.I)
    _secret_value = re.compile(
        r"((?:password|passwd|secret|token|credential|authorization|ssid)\s*[:=]\s*)\S+",
        re.I,
    )

    def __init__(self, path: pathlib.Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self._file = path.open("a", encoding="utf-8", buffering=1)

    @classmethod
    def _redact(cls, value: Any, key: str = "") -> Any:
        if cls._secret_key.search(key):
            return "[REDACTED]"
        if isinstance(value, dict):
            return {k: cls._redact(v, k) for k, v in value.items()}
        if isinstance(value, list):
            return [cls._redact(item) for item in value]
        if isinstance(value, str):
            return cls._secret_value.sub(r"\1[REDACTED]", value)
        return value

    def write(self, entry: dict[str, Any]) -> None:
        self._file.write(json.dumps(self._redact(entry), separators=(",", ":")) + "\n")

    def close(self) -> None:
        self._file.close()


async def reboot_controller(http: Http, timeout: float) -> None:
    print(head("Requesting controller reboot..."))
    await http.post("/system", {"cmd": "restart"})
    deadline = time.monotonic() + timeout
    probe = Http(http.base_url, None, min(http.timeout, 1.0), http.api_log)
    probe.headers = http.headers.copy()
    offline = False
    last_error = ""
    while time.monotonic() < deadline:
        try:
            info = await probe.get("/info")
        except OSError as err:
            if not offline:
                print(dim("Controller offline; waiting for startup..."))
            offline = True
            last_error = str(err)
        else:
            if offline:
                if not isinstance(info, dict) or not isinstance(info.get("app"), dict):
                    raise RuntimeError("Controller returned invalid /info after reboot")
                print(ok("Controller back online; starting light discovery."))
                return
        await asyncio.sleep(min(0.2, max(0.0, deadline - time.monotonic())))
    detail = f"last connection error: {last_error}" if offline else "controller never went offline"
    raise RuntimeError(f"Reboot not confirmed within {timeout:g}s: {detail}; tests were not started")


# --------------------------------------------------------------------------- WebSocket


class WsClient:
    """JSON-RPC WebSocket client that records ``light_state`` notifications.

    Firmware without the multiplexed ``lights`` channel only broadcasts the
    unindexed legacy ``color_event`` for the default light; in that case the
    events are attributed to ``legacy_light``.
    """

    def __init__(self, url: str, password: str | None, verbose: bool):
        self.legacy_light: Light | None = None
        self.url = url
        self.password = password or ""
        self.verbose = verbose
        self.ws: Any = None
        self._ids = itertools.count(1)
        self._pending: dict[int, asyncio.Future] = {}
        self._reader: asyncio.Task | None = None
        self.events: list[Event] = []
        self.transitions: list[tuple[float, dict]] = []
        self._event_signal = asyncio.Event()

    async def connect(self) -> None:
        self.ws = await websockets.connect(self.url, ping_interval=10, ping_timeout=10, max_size=None)
        self._reader = asyncio.create_task(self._read_loop())

    async def close(self) -> None:
        if self._reader:
            self._reader.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self._reader
        if self.ws:
            await self.ws.close()

    async def _read_loop(self) -> None:
        async for message in self.ws:
            try:
                msg = json.loads(message)
            except json.JSONDecodeError:
                continue
            now = time.monotonic()
            if "id" in msg and msg["id"] in self._pending and ("result" in msg or "error" in msg):
                fut = self._pending.pop(msg["id"])
                if not fut.done():
                    fut.set_result(msg)
                continue
            method = msg.get("method")
            params = msg.get("params") or {}
            if method == "light_state":
                self.add_event(Event(now, int(params.get("light", -1)), params.get("light_id", ""),
                                     params.get("state", {})))
            elif method == "color_event" and self.legacy_light is not None:
                lt = self.legacy_light
                self.add_event(Event(now, lt.index, lt.id, params, "color_event"))
            elif method == "transition_finished":
                self.transitions.append((now, params))
                self._event_signal.set()
                if self.verbose:
                    print(dim(f"      ws transition_finished {params}"))
            elif method == "lights_changed":
                print(dim(f"      ws lights_changed {params} (configuration changed during the run)"))

    def add_event(self, ev: Event) -> None:
        self.events.append(ev)
        self._event_signal.set()
        if self.verbose:
            print(dim(f"      {ev.source} light={ev.light} {fmt_state(ev.state)}"))

    async def call(self, method: str, params: dict | None = None, timeout: float = 5.0) -> dict:
        req_id = next(self._ids)
        fut = asyncio.get_running_loop().create_future()
        self._pending[req_id] = fut
        await self.ws.send(json.dumps({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params or {}}))
        try:
            return await asyncio.wait_for(fut, timeout)
        finally:
            self._pending.pop(req_id, None)

    async def call_authenticated(self, method: str, params: dict | None = None) -> dict:
        reply = await self.call(method, params)
        err = reply.get("error") or {}
        if err.get("code") != -32001:
            return reply
        challenge = err.get("challenge")
        if not challenge:
            raise RuntimeError(f"WebSocket requires authentication but sent no challenge: {err}")
        digest = hashlib.sha256(f"{challenge}:{self.password}".encode()).hexdigest()
        auth = await self.call("authenticate", {"hash": digest})
        if "error" in auth:
            raise RuntimeError(f"WebSocket authentication failed: {auth['error']}")
        return await self.call(method, params)

    async def subscribe_lights(self) -> bool:
        """Subscribe to the multiplexed ``lights`` channel; False on firmware without it."""
        reply = await self.call_authenticated("subscribe", {"channel": "lights"})
        if "error" not in reply:
            return True
        if reply["error"].get("code") == -32602:
            return False
        raise RuntimeError(f"subscribe(lights) rejected: {reply['error']}")

    def events_for(self, light: Light, since: float) -> list[Event]:
        return [e for e in self.events if e.light == light.index and e.t >= since]

    async def wait_for_event(self, predicate, timeout: float) -> Event | None:
        deadline = time.monotonic() + timeout
        seen = 0
        while True:
            for ev in self.events[seen:]:
                if predicate(ev):
                    return ev
            seen = len(self.events)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self._event_signal.clear()
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(self._event_signal.wait(), remaining)

    async def wait_for_transition(self, name: str, since: float, timeout: float) -> float | None:
        deadline = time.monotonic() + timeout
        while True:
            for t, params in self.transitions:
                if t >= since and params.get("name") == name:
                    return t
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self._event_signal.clear()
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(self._event_signal.wait(), remaining)


# --------------------------------------------------------------------------- state helpers


def unwrap_color(payload: Any) -> dict:
    """Return the bare ``{"hsv": ...}`` / ``{"raw": ...}`` object from a /color reply."""
    if isinstance(payload, dict):
        if "result" in payload and isinstance(payload["result"], dict):
            payload = payload["result"]
        if "hsv" in payload or "raw" in payload:
            return payload
    raise RuntimeError(f"unexpected /color payload: {payload!r}")


def fmt_state(state: dict) -> str:
    if "hsv" in state:
        hsv = state["hsv"]
        return "hsv(" + ", ".join(f"{k}={hsv[k]:g}" for k in ("h", "s", "v", "ct") if k in hsv) + ")"
    if "raw" in state:
        return f"raw({state['raw']})"
    return str(state)


def hue_diff(a: float, b: float) -> float:
    d = abs(a - b) % 360
    return min(d, 360 - d)


def compare_hsv(light: Light, expected: dict, state: dict) -> tuple[bool, str]:
    actual = state.get("hsv")
    if actual is None:
        return False, f"expected hsv, got {fmt_state(state)}"
    problems = []
    keys = ["v"]
    if light.has_rgb:
        keys += ["s"]
        if expected.get("s", 0) > 0 and expected.get("v", 0) > 0:
            keys += ["h"]
    if light.has_ct and "ct" in expected:
        keys += ["ct"]
    for key in keys:
        if key not in expected:
            continue
        if key not in actual:
            problems.append(f"{key} missing")
            continue
        diff = hue_diff(actual[key], expected[key]) if key == "h" else abs(actual[key] - expected[key])
        if diff > TOL[key]:
            problems.append(f"{key}={actual[key]:g} (want {expected[key]:g})")
    return not problems, ("; ".join(problems) or fmt_state(state))


def compare_raw(expected: dict | list, state: dict) -> tuple[bool, str]:
    actual = state.get("raw")
    if actual is None:
        return False, f"expected raw, got {fmt_state(state)}"
    problems = []
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return False, f"expected named raw object, got {actual}"
        for key, want in expected.items():
            got = actual.get(key)
            if got is None or abs(got - want) > TOL["raw"]:
                problems.append(f"{key}={got} (want {want})")
    else:
        if not isinstance(actual, list):
            return False, f"expected raw array, got {actual}"
        for i, want in enumerate(expected):
            got = actual[i] if i < len(actual) else None
            if got is None or abs(got - want) > TOL["raw"]:
                problems.append(f"[{i}]={got} (want {want})")
    return not problems, ("; ".join(problems) or fmt_state(state))


def compare(light: Light, mode: str, expected: Any, state: dict) -> tuple[bool, str]:
    return compare_hsv(light, expected, state) if mode == "hsv" else compare_raw(expected, state)


def scalar(light: Light, mode: str, state: dict, key: str) -> float | None:
    """Extract the tracked fade component from a state object."""
    if mode == "hsv":
        return state.get("hsv", {}).get(key)
    raw = state.get("raw")
    if isinstance(raw, dict):
        return raw.get(key)
    if isinstance(raw, list) and isinstance(key, int) and key < len(raw):
        return raw[key]
    return None


# --------------------------------------------------------------------------- test plans


def solid_plan(light: Light) -> list[tuple[str, str, Any]]:
    """(description, mode, value) steps appropriate for the light type."""
    steps: list[tuple[str, str, Any]] = []
    mid_ct = (light.ct_ww + light.ct_cw) // 2
    if light.has_rgb:
        steps += [
            ("hsv red", "hsv", {"h": 0, "s": 100, "v": 80}),
            ("hsv green", "hsv", {"h": 120, "s": 100, "v": 60}),
            ("hsv blue", "hsv", {"h": 240, "s": 100, "v": 40}),
            ("hsv pastel", "hsv", {"h": 300, "s": 40, "v": 70}),
        ]
        if light.has_ct:
            steps += [
                ("hsv white warm", "hsv", {"h": 0, "s": 0, "v": 70, "ct": light.ct_ww}),
                ("hsv white cold", "hsv", {"h": 0, "s": 0, "v": 70, "ct": light.ct_cw}),
            ]
        else:
            steps += [("hsv white", "hsv", {"h": 0, "s": 0, "v": 70})]
        channels = NAMED_CHANNELS[light.type]
        steps.append(("raw mixed", "raw", {ch: (i + 1) * 1023 // (len(channels) + 1) for i, ch in enumerate(channels)}))
        steps.append(("raw single channel", "raw", {ch: (1023 if ch == channels[0] else 0) for ch in channels}))
    elif light.type == "WWCW":
        steps += [
            ("hsv warm", "hsv", {"v": 80, "ct": light.ct_ww}),
            ("hsv mid ct", "hsv", {"v": 50, "ct": mid_ct}),
            ("hsv cold", "hsv", {"v": 30, "ct": light.ct_cw}),
            ("raw ww+cw", "raw", [700, 300]),
            ("raw cw only", "raw", [0, 900]),
        ]
    else:  # WW, CW, SINGLE
        steps += [
            ("hsv val 80", "hsv", {"v": 80}),
            ("hsv val 25", "hsv", {"v": 25}),
            ("raw 900", "raw", [900]),
            ("raw 100", "raw", [100]),
        ]
    steps.append(("hsv off (v=0)", "hsv", {"v": 0} if not light.has_rgb else {"h": 0, "s": 0, "v": 0}))
    return steps


def fade_plan(light: Light) -> list[tuple[str, str, Any, Any, Any]]:
    """(description, mode, start, target, tracked key) fade steps."""
    fades: list[tuple[str, str, Any, Any, Any]] = []
    if light.has_rgb:
        fades.append(("fade val 5->95", "hsv", {"h": 30, "s": 100, "v": 5}, {"h": 30, "s": 100, "v": 95}, "v"))
        fades.append(("fade hue 0->120", "hsv", {"h": 0, "s": 100, "v": 60}, {"h": 120, "s": 100, "v": 60}, "h"))
        fades.append(("fade sat 0->100", "hsv", {"h": 200, "s": 0, "v": 60}, {"h": 200, "s": 100, "v": 60}, "s"))
        ch = NAMED_CHANNELS[light.type][0]
        fades.append((f"fade raw {ch} 0->1000", "raw",
                      {c: 0 for c in NAMED_CHANNELS[light.type]},
                      {**{c: 0 for c in NAMED_CHANNELS[light.type]}, ch: 1000}, ch))
    else:
        start = {"v": 5}
        target = {"v": 95}
        if light.has_ct:
            start["ct"] = target["ct"] = (light.ct_ww + light.ct_cw) // 2
        fades.append(("fade val 5->95", "hsv", start, target, "v"))
        n = INDEXED_LENGTH[light.type]
        fades.append(("fade raw[0] 0->1000", "raw", [0] * n, [1000] + [0] * (n - 1), 0))
    if light.has_ct:
        fades.append(("fade ct warm->cold", "hsv",
                      {"h": 0, "s": 0, "v": 60, "ct": light.ct_ww},
                      {"h": 0, "s": 0, "v": 60, "ct": light.ct_cw}, "ct"))
    return fades


def soak_commands(light: Light, intensity_max: int) -> list[dict[str, Any]]:
    """Build 30 up/down fade pairs, queued internally for a five-minute cycle."""
    channel_count = len(NAMED_CHANNELS[light.type]) if light.has_rgb else INDEXED_LENGTH[light.type]
    groups = [[index] for index in range(channel_count)]
    if channel_count > 1:
        groups.append(list(range(channel_count)))

    max_raw = round(1023 * intensity_max / 100)
    commands = []
    for pair in range(30):
        group = groups[pair % len(groups)]
        duration_ms = 1000 if pair < 15 else 8000
        peak = [max_raw if index in group else 0 for index in range(channel_count)]
        for values in (peak, [0] * channel_count):
            commands.append({
                "cmd": "fade",
                "raw": values,
                "t": duration_ms,
                "q": "back",
                "light": light.index,
                "light_id": light.id,
            })
    return commands


# --------------------------------------------------------------------------- exerciser


class Exerciser:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.api_log = ApiLog(args.api_log)
        scheme = "https" if args.tls else "http"
        ws_scheme = "wss" if args.tls else "ws"
        self.http = Http(f"{scheme}://{args.host}", args.password, args.timeout, self.api_log)
        self.ws = WsClient(f"{ws_scheme}://{args.host}/ws", args.password, args.verbose)
        self.log_http = Http(args.log_service, None, args.timeout, self.api_log)
        self.results = Results()
        self.legacy = False
        self._names = itertools.count(1)

    # --- discovery

    async def discover(self) -> list[Light]:
        config = await self.http.get("/config")
        hw = (config.get("hardware") or {}).get("configuration") or {}
        lights = []
        for entry in hw.get("lights") or []:
            ct = (entry.get("color") or {}).get("colortemp") or {}
            lights.append(Light(
                index=int(entry.get("index", 65535)),
                id=entry.get("id", ""),
                name=entry.get("name", ""),
                type=entry.get("type", "RGB"),
                is_default=bool(entry.get("is_default", False)),
                ct_ww=int(ct.get("ww", 2700)),
                ct_cw=int(ct.get("cw", 6000)),
            ))
        return lights

    # --- primitives

    def command(self, light: Light, mode: str, value: Any, cmd: str = "solid", t: int = 0,
                name: str | None = None) -> dict:
        body: dict[str, Any] = {"cmd": cmd, mode: value, "t": t, "q": "single",
                                "light": light.index, "light_id": light.id}
        if name:
            body["name"] = name
        return body

    async def read_color(self, light: Light) -> dict:
        return unwrap_color(await self.http.get(f"/color?light={light.index}"))

    async def send(self, body: dict) -> None:
        if self.args.via == "ws":
            reply = await self.ws.call_authenticated("color", body)
            if "error" in reply:
                raise RuntimeError(f"color command rejected: {reply['error']}")
        else:
            await self.http.post(f"/color?light={body['light']}", body)

    # --- solid steps

    async def run_solid(self, light: Light) -> None:
        print(head("  solid commands"))
        for desc, mode, value in solid_plan(light):
            print(f"   {desc}: {mode}={value}")
            since = time.monotonic()
            try:
                await self.send(self.command(light, mode, value))
            except RuntimeError as err:
                self.results.check(False, f"{light.index}/{desc} accepted", str(err))
                continue

            ev = await self.ws.wait_for_event(
                lambda e: e.light == light.index and e.t >= since and compare(light, mode, value, e.state)[0],
                self.args.event_timeout)
            if ev is not None:
                self.results.check(True, f"{light.index}/{desc} observed via {ev.source}",
                                   f"{fmt_state(ev.state)} after {(ev.t - since) * 1000:.0f} ms")
            else:
                seen = self.ws.events_for(light, since)
                last = fmt_state(seen[-1].state) if seen else "no event"
                self.results.check(False, f"{light.index}/{desc} observed via {self.observer(light)}",
                                   f"last: {last}")
                if any(e.light_id != light.id for e in seen):
                    print(bad(f"      light_id mismatch in events: {[e.light_id for e in seen]}"))

            state = await self.read_color(light)
            match, detail = compare(light, mode, value, state)
            self.results.check(match, f"{light.index}/{desc} GET /color", detail)
            await asyncio.sleep(self.args.dwell)

    # --- fades

    async def run_fades(self, light: Light) -> None:
        duration = self.args.fade_ms
        print(head(f"  timed fades ({duration} ms)"))
        for desc, mode, start, target, key in fade_plan(light):
            print(f"   {desc}: {mode} {start} -> {target}")
            await self.send(self.command(light, mode, start))
            await self.ws.wait_for_event(
                lambda e: e.light == light.index and compare(light, mode, start, e.state)[0], self.args.event_timeout)
            await asyncio.sleep(0.4)  # let the event rate limiter (5/s per light) settle

            name = f"probe-{light.index}-{next(self._names)}"
            since = time.monotonic()
            try:
                await self.send(self.command(light, mode, target, cmd="fade", t=duration, name=name))
            except RuntimeError as err:
                self.results.check(False, f"{light.index}/{desc} accepted", str(err))
                continue

            done_at = await self.ws.wait_for_transition(name, since, duration / 1000 + self.args.event_timeout)
            await self.ws.wait_for_event(
                lambda e: e.light == light.index and e.t >= since and compare(light, mode, target, e.state)[0],
                self.args.event_timeout if done_at is None else 1.0)
            self.analyse_fade(light, desc, mode, start, target, key, since, done_at)

            state = await self.read_color(light)
            match, detail = compare(light, mode, target, state)
            self.results.check(match, f"{light.index}/{desc} final GET /color", detail)
            await asyncio.sleep(self.args.dwell)

    def analyse_fade(self, light: Light, desc: str, mode: str, start: Any, target: Any, key: Any,
                     since: float, done_at: float | None) -> None:
        events = self.ws.events_for(light, since)
        values = [(e.t - since, scalar(light, mode, e.state, key)) for e in events]
        values = [(t, v) for t, v in values if v is not None]
        duration = self.args.fade_ms / 1000
        tag = f"{light.index}/{desc}"

        if self.args.verbose or not values:
            trace = ", ".join(f"{t * 1000:.0f}ms:{v:g}" for t, v in values) or "none"
            print(dim(f"      trace {key}: {trace}"))

        lo = start[key]
        hi = target[key]
        span = abs(hi - lo)
        intermediate = [v for _, v in values if min(lo, hi) + span * 0.05 < v < max(lo, hi) - span * 0.05]
        expected_min = max(2, int(duration))  # legacy color_event runs at ~2/s, light_state at up to 5/s
        self.results.check(len(intermediate) >= expected_min, f"{tag} intermediate updates ({self.observer(light)})",
                           f"{len(intermediate)} intermediate of {len(values)} events (want >= {expected_min})")

        direction = 1 if hi >= lo else -1
        regressions = sum(1 for (_, a), (_, b) in zip(values, values[1:]) if (b - a) * direction < -TOL.get(str(key), TOL["raw"]))
        if key == "h":
            regressions = 0  # hue may legitimately wrap; monotonicity is not meaningful on a circle
        self.results.check(regressions == 0, f"{tag} monotonic progress", f"{regressions} regressions")

        reached = next((t for t, v in values if abs(v - hi) <= TOL.get(str(key), TOL["raw"])), None)
        end_time = done_at - since if done_at is not None else reached
        if done_at is not None:
            print(dim(f"      transition_finished after {(done_at - since) * 1000:.0f} ms"))
        if end_time is None:
            self.results.check(False, f"{tag} completed", "target never observed on websocket")
        else:
            lower, upper = duration * 0.8, duration + max(0.6, duration * 0.2)
            self.results.check(lower <= end_time <= upper, f"{tag} duration",
                               f"{end_time * 1000:.0f} ms (expected {duration * 1000:.0f} ms)")

        if values:
            first_t = values[0][0]
            self.results.check(first_t < min(1.0, duration / 2), f"{tag} fade started promptly",
                               f"first update after {first_t * 1000:.0f} ms")

    # --- restore

    async def restore(self, light: Light, original: dict) -> None:
        mode = "hsv" if "hsv" in original else "raw"
        value = original[mode]
        if mode == "hsv":
            value = {k: v for k, v in value.items() if k in ("h", "s", "v", "ct")}
        try:
            await self.send(self.command(light, mode, value))
            print(dim(f"  restored {fmt_state(original)}"))
        except RuntimeError as err:
            print(bad(f"  could not restore original state: {err}"))

    # --- observation

    def observer(self, light: Light) -> str:
        if not self.legacy:
            return "light_state"
        return "color_event" if light.is_default else "http-poll"

    async def poll(self, light: Light) -> None:
        """Feed GET /color readings into the event stream for lights without websocket events."""
        while True:
            with contextlib.suppress(RuntimeError, OSError):
                state = await self.read_color(light)
                self.ws.add_event(Event(time.monotonic(), light.index, light.id, state, "http-poll"))
            await asyncio.sleep(self.args.poll_interval)

    # --- main

    async def run_permissive(self) -> int:
        lights = await self.discover()
        if self.args.light:
            lights = [light for light in lights if light.index in self.args.light]
        if not lights:
            raise RuntimeError("no matching virtual lights configured")
        print(head(f"Permissive HTTP exercise: {len(lights)} lights; no reboot or validation"))
        errors = 0

        async def send(body: dict) -> None:
            nonlocal errors
            try:
                reply = await self.http.post(f"/color?light={body['light']}", body)
                if isinstance(reply, dict) and (reply.get("success") is False or "error" in reply):
                    raise RuntimeError(f"command rejected: {reply}")
            except (RuntimeError, OSError, ValueError) as err:
                errors += 1
                print(bad(f"  light {body['light']} command failed; continuing: {err}"))

        if self.args.soak_hours is not None:
            deadline = time.monotonic() + self.args.soak_hours * 3600
            cycle = 0
            try:
                while time.monotonic() < deadline:
                    cycle += 1
                    print(head(f"Cycle {cycle}: uploading queued fades"))
                    for light in lights:
                        channels = len(NAMED_CHANNELS[light.type]) if light.has_rgb else INDEXED_LENGTH[light.type]
                        await send(self.command(light, "raw", [0] * channels))
                        for command in soak_commands(light, self.args.intensity_max):
                            await send(command)
                    await asyncio.sleep(min(self.args.cycle_minutes * 60,
                                            max(0, deadline - time.monotonic())))
            finally:
                for light in lights:
                    channels = len(NAMED_CHANNELS[light.type]) if light.has_rgb else INDEXED_LENGTH[light.type]
                    await send(self.command(light, "raw", [0] * channels))
        else:
            for light in lights:
                print(head(f"\n== {light.label()}"))
                original = None
                if not self.args.no_restore:
                    try:
                        original = await self.read_color(light)
                    except (RuntimeError, OSError, ValueError) as err:
                        errors += 1
                        print(bad(f"  cannot snapshot light {light.index}; continuing without restoration: {err}"))
                try:
                    if not self.args.skip_solid:
                        for desc, mode, value in solid_plan(light):
                            print(f"  {desc}")
                            await send(self.command(light, mode, value))
                            await asyncio.sleep(self.args.dwell)
                    if not self.args.skip_fades:
                        for desc, mode, start, target, _ in fade_plan(light):
                            print(f"  {desc}")
                            await send(self.command(light, mode, start))
                            await asyncio.sleep(self.args.dwell)
                            await send(self.command(light, mode, target, cmd="fade", t=self.args.fade_ms))
                            await asyncio.sleep(self.args.fade_ms / 1000 + self.args.dwell)
                finally:
                    if original is not None:
                        mode = "hsv" if "hsv" in original else "raw"
                        await send(self.command(light, mode, original[mode]))
        print(head(f"Permissive exercise complete: {errors} errors reported; validation skipped"))
        return 0

    async def run(self) -> int:
        if self.args.permissive:
            return await self.run_permissive()
        if self.args.soak_hours is not None:
            return await self.run_soak()
        if not self.args.no_reboot:
            await reboot_controller(self.http, self.args.reboot_timeout)
        lights = await self.discover()
        if not lights:
            print(bad("no virtual lights configured"))
            return 2
        print(head(f"{len(lights)} virtual light(s) on {self.args.host}:"))
        for light in lights:
            print(f"  {light.label()} ct={light.ct_ww}..{light.ct_cw}K")
        if self.args.light:
            lights = [lt for lt in lights if lt.index in self.args.light]
            if not lights:
                print(bad(f"none of the requested lights {self.args.light} exist"))
                return 2

        await self.ws.connect()
        try:
            self.legacy = not await self.ws.subscribe_lights()
            if self.legacy:
                default = next((lt for lt in lights if lt.is_default), None)
                self.ws.legacy_light = default
                print(bad("firmware has no 'lights' websocket channel; falling back to legacy color_event "
                          f"for the default light and polling GET /color every {self.args.poll_interval}s "
                          "for the others"))
            else:
                print(dim("subscribed to websocket 'lights' channel"))
            for light in lights:
                print(head(f"\n== {light.label()}"))
                original = await self.read_color(light)
                print(f"  initial GET /color: {fmt_state(original)}")
                poller = asyncio.create_task(self.poll(light)) if self.observer(light) == "http-poll" else None
                try:
                    if not self.args.skip_solid:
                        await self.run_solid(light)
                    if not self.args.skip_fades:
                        await self.run_fades(light)
                finally:
                    if poller:
                        poller.cancel()
                        with contextlib.suppress(asyncio.CancelledError):
                            await poller
                    if not self.args.no_restore:
                        await self.restore(light, original)
        finally:
            await self.ws.close()

        r = self.results
        print(head(f"\nSummary: {r.passed} passed, {r.failed} failed"))
        for failure in r.failures:
            print(bad(f"  - {failure}"))
        return 0 if r.failed == 0 else 1

    async def run_soak(self) -> int:
        controller_ip = self.args.log_ip or self.args.host.split(":", 1)[0]
        health = await self.log_http.get("/api/v1/health")
        if not isinstance(health, dict) or health.get("status") != "ok":
            raise RuntimeError(f"log service health check failed: {health!r}")

        cursor_result = await self.log_http.get(
            f"/api/v1/logs?ip={controller_ip}&limit=1"
        )
        baseline_id = max(
            (int(entry["id"]) for entry in cursor_result.get("items", [])),
            default=0,
        )
        started = datetime.datetime.now(datetime.timezone.utc)
        self.api_log.write({
            "kind": "run_start",
            "timestamp": started.isoformat(),
            "controller": self.http.base_url,
            "controller_ip": controller_ip,
            "log_service": self.args.log_service,
            "baseline_log_id": baseline_id,
            "duration_hours": self.args.soak_hours,
            "cycle_minutes": self.args.cycle_minutes,
            "maximum_intensity_percent": self.args.intensity_max,
            "fast_fade_ms": 1000,
            "slow_fade_ms": 8000,
            "api_log": str(self.args.api_log.resolve()),
        })

        lights = await self.discover()
        if self.args.light:
            lights = [light for light in lights if light.index in self.args.light]
        if not lights:
            raise RuntimeError("no matching virtual lights configured")

        max_raw = round(1023 * self.args.intensity_max / 100)
        print(head(f"Low-intensity soak: {len(lights)} light(s), max {self.args.intensity_max}% "
                   f"(raw {max_raw}/1023)"))
        print(f"  Controller: {self.http.base_url}")
        print(f"  Log service: {self.args.log_service} (baseline id {baseline_id})")
        print(f"  Timestamped API log: {self.args.api_log}")
        for light in lights:
            print(f"  {light.label()}: 60 internally queued fades per cycle")

        cycle_seconds = self.args.cycle_minutes * 60
        duration_seconds = self.args.soak_hours * 3600
        started_mono = time.monotonic()
        deadline = started_mono + duration_seconds
        next_cycle = started_mono
        log_cursor = baseline_id
        cycle = 0

        async def collect_logs() -> None:
            nonlocal log_cursor
            while True:
                page = await self.log_http.get(
                    f"/api/v1/logs?ip={controller_ip}&limit=2000&from={log_cursor + 1}"
                )
                entries = page.get("items", [])
                for entry in entries:
                    log_cursor = max(log_cursor, int(entry["id"]))
                    self.api_log.write({
                        "kind": "syslog",
                        "timestamp": entry.get("receivedAt"),
                        "log_id": entry["id"],
                        "controller_ip": entry.get("sourceIp", controller_ip),
                        "boot": entry.get("boot"),
                        "device_time": entry.get("deviceTime"),
                        "message": entry.get("message"),
                    })
                if not page.get("nextAfter"):
                    break

        try:
            while time.monotonic() < deadline:
                now = time.monotonic()
                if now >= next_cycle and deadline - now >= cycle_seconds:
                    cycle += 1
                    print(head(f"Cycle {cycle}: uploading queued fade animations"))
                    for light in lights:
                        raw_channels = len(NAMED_CHANNELS[light.type]) if light.has_rgb else INDEXED_LENGTH[light.type]
                        zero = [0] * raw_channels
                        await self.send(self.command(light, "raw", zero))
                        commands = soak_commands(light, self.args.intensity_max)
                        for offset in range(0, len(commands), 10):
                            await self.http.post(
                                "/color",
                                {"cmds": commands[offset:offset + 10]},
                            )
                    next_cycle = started_mono + cycle * cycle_seconds

                await asyncio.sleep(min(30.0, max(0.0, deadline - time.monotonic())))
                for light in lights:
                    await self.read_color(light)
                try:
                    await collect_logs()
                except (RuntimeError, OSError) as err:
                    print(bad(f"Log-service poll failed: {err}"))
                    self.api_log.write({
                        "kind": "log_poll_error",
                        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                        "error": str(err),
                    })
        finally:
            for light in lights:
                raw_channels = len(NAMED_CHANNELS[light.type]) if light.has_rgb else INDEXED_LENGTH[light.type]
                try:
                    await self.send(self.command(light, "raw", [0] * raw_channels))
                except (RuntimeError, OSError) as err:
                    print(bad(f"Could not stop light {light.index} cleanly: {err}"))

        self.api_log.write({
            "kind": "run_end",
            "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "cycles": cycle,
            "last_log_id": log_cursor,
        })
        print(head(f"Soak complete: {cycle} cycle(s); correlated logs through id {log_cursor}"))
        return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host", help="controller host[:port], e.g. 192.168.1.50 or lightinator.local")
    p.add_argument("--password", help="API password if the API is secured")
    p.add_argument("--light", type=int, action="append", help="only test this light index (repeatable)")
    p.add_argument("--soak-hours", type=float, help="run low-intensity queued fade cycles for this many hours")
    p.add_argument("--cycle-minutes", type=float, default=5.0,
                   help="soak cycle interval in minutes (default 5)")
    p.add_argument("--intensity-max", type=int, default=10,
                   help="soak maximum output percentage, limited to 0..10 (default 10)")
    p.add_argument("--log-service", default="http://192.168.29.5:4821",
                   help="log service base URL")
    p.add_argument("--log-ip", help="controller IP used to query correlated syslog")
    p.add_argument("--api-log", type=pathlib.Path,
                   default=pathlib.Path.cwd() /
                   f"virtual-light-exerciser-{datetime.datetime.now().strftime('%Y%m%d-%H%M%S')}.jsonl",
                   help="append timestamped API and correlated syslog JSONL here")
    p.add_argument("--via", choices=["http", "ws"], default="http",
                   help="send color commands via HTTP POST /color (default) or JSON-RPC over the websocket")
    p.add_argument("--fade-ms", type=int, default=3000, help="duration of timed fades (default 3000)")
    p.add_argument("--dwell", type=float, default=0.5, help="pause between steps in seconds (default 0.5)")
    p.add_argument("--event-timeout", type=float, default=3.0,
                   help="seconds to wait for a matching websocket event (default 3)")
    p.add_argument("--timeout", type=float, default=5.0, help="HTTP timeout in seconds (default 5)")
    p.add_argument("--no-reboot", action="store_true", help="skip the initial controller reboot")
    p.add_argument("--permissive", action="store_true",
                   help="HTTP exercise only: skip reboot/validation/log collection; report errors and continue")
    p.add_argument("--reboot-timeout", type=float, default=60.0,
                   help="maximum wait for controller offline/online transition (default 60 seconds)")
    p.add_argument("--poll-interval", type=float, default=0.2,
                   help="GET /color poll interval for lights without websocket events (legacy firmware)")
    p.add_argument("--skip-solid", action="store_true", help="skip the solid color steps")
    p.add_argument("--skip-fades", action="store_true", help="skip the timed fade steps")
    p.add_argument("--no-restore", action="store_true", help="leave the lights in their final test state")
    p.add_argument("--tls", action="store_true", help="use https/wss")
    p.add_argument("--no-color", action="store_true", help="disable ANSI colors")
    p.add_argument("-v", "--verbose", action="store_true", help="print every websocket event and fade trace")
    args = p.parse_args(argv)
    if args.reboot_timeout <= 0 or args.timeout <= 0:
        p.error("--reboot-timeout and --timeout must be positive")
    if args.soak_hours is not None and args.soak_hours <= 0:
        p.error("--soak-hours must be positive")
    if args.cycle_minutes <= 0:
        p.error("--cycle-minutes must be positive")
    if not 0 <= args.intensity_max <= 10:
        p.error("--intensity-max must be between 0 and 10")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.no_color:
        Colors.enabled = False
    exerciser = Exerciser(args)
    try:
        return asyncio.run(exerciser.run())
    except KeyboardInterrupt:
        return 130
    except (RuntimeError, OSError) as err:
        print(bad(f"error: {err}"))
        return 2
    finally:
        exerciser.api_log.close()


if __name__ == "__main__":
    sys.exit(main())
