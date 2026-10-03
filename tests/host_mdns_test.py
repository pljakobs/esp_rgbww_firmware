from __future__ import annotations

import json
import os
import socket
import statistics
import sys
import time
from collections import Counter
from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
zeroconf = pytest.importorskip("zeroconf", reason="mDNS tests require pip install zeroconf")

from virtual_controllers import BASE_CHIP_ID, VirtualController, controller_ip
from host_smoke_api_test import (
    SmokeConfig, append_jsonl, http_request, recv_ws_text, smoke_config,
    websocket_handshake, ws_request, write_json,
)


def ramp_counts(value: str) -> list[int]:
    counts = [int(item.strip()) for item in value.split(",")]
    if len(counts) < 2 or counts[0] != 0 or any(left >= right for left, right in zip(counts, counts[1:])):
        raise ValueError("ramp must start at zero and increase strictly")
    return counts


def host_records(payload: Any) -> list[dict[str, Any]]:
    if not isinstance(payload, dict) or not isinstance(payload.get("hosts"), list):
        raise AssertionError("/hosts must return an object containing a hosts array")
    records = payload["hosts"]
    if any(not isinstance(row, dict) for row in records):
        raise AssertionError("host records must be objects")
    identities = [int(row["id"]) for row in records]
    duplicates = [identity for identity, count in Counter(identities).items() if count > 1]
    if duplicates:
        raise AssertionError(f"duplicate controller IDs: {duplicates}")
    return records


def check_hosts(payload: Any, controllers: list[VirtualController], visible_ids: set[int], *, all_hosts: bool) -> None:
    records = host_records(payload)
    fleet_ids = {controller.chip_id for controller in controllers}
    actual = {int(row["id"]): row for row in records if int(row["id"]) in fleet_ids}
    expected_ids = fleet_ids if all_hosts else visible_ids
    if set(actual) != expected_ids:
        raise AssertionError(f"expected IDs {sorted(expected_ids)}, got {sorted(actual)}")
    for controller in controllers:
        if controller.chip_id not in actual:
            continue
        row = actual[controller.chip_id]
        hostname = row["hostname"].rstrip(".").removesuffix(".local")
        assert hostname == controller.hostname_raw, f"wrong hostname for {controller.chip_id}"
        assert row["ip_address"] == controller.ip_address, f"wrong IP for {controller.chip_id}"
        if controller.chip_id in visible_ids:
            assert row["host_type"] == "CONTROLLER", f"wrong host type for {controller.chip_id}"
        assert row["visible"] is (controller.chip_id in visible_ids), f"wrong visibility for {controller.chip_id}"
        assert row["state"] == (3 if controller.chip_id in visible_ids else 2), f"wrong state for {controller.chip_id}"


def read_json(url: str) -> dict[str, Any]:
    response = http_request("GET", url)
    if response["error"] or response["status"] != 200:
        raise AssertionError(f"GET {url}: status={response['status']} error={response['error']}")
    payload = json.loads(response["body"])
    if not isinstance(payload, dict):
        raise AssertionError(f"GET {url} did not return an object")
    return payload


def settle_for_ttl(ttl: int, refresh: Callable[[], None] | None = None) -> None:
    deadline = time.monotonic() + 2 * ttl
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        if refresh is not None:
            refresh()
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(1, remaining))


def wait_for_hosts(base_url: str, controllers: list[VirtualController], visible_ids: set[int], timeout: float,
                   refresh: Callable[[], None] | None = None, label: str = "inventory") -> None:
    deadline = time.monotonic() + timeout
    started = time.monotonic()
    next_progress = started + 30
    last_error = "no response"
    while time.monotonic() < deadline:
        if refresh is not None:
            refresh()
        try:
            check_hosts(read_json(f"{base_url}/hosts"), controllers, visible_ids, all_hosts=False)
            check_hosts(read_json(f"{base_url}/hosts?all=true"), controllers, visible_ids, all_hosts=True)
            print(f"{label}: hosts converged in {time.monotonic() - started:.1f}s", flush=True)
            return
        except (AssertionError, KeyError, ValueError) as exc:
            last_error = str(exc)
        now = time.monotonic()
        if now >= next_progress:
            print(f"{label}: still waiting after {now - started:.0f}s: {last_error}", flush=True)
            next_progress = now + 30
        time.sleep(1)
    raise AssertionError(f"mDNS did not converge within {timeout}s: {last_error}")


def change_counters_from_info(info: dict[str, Any]) -> tuple[int, int]:
    debug = info.get("debug")
    if not isinstance(debug, dict) or "mdns_hostname_changes" not in debug or "mdns_ip_changes" not in debug:
        raise AssertionError("firmware /info debug section lacks mdns_hostname_changes / mdns_ip_changes counters")
    return int(debug["mdns_hostname_changes"]), int(debug["mdns_ip_changes"])


def read_change_counters(base_url: str) -> tuple[int, int]:
    return change_counters_from_info(read_json(f"{base_url}/info?v=2&sparse=false"))


def runtime_samples(sock: socket.socket, count: int, timeout: float, after_uptime: int = 0,
                    refresh: Callable[[], None] | None = None) -> list[dict[str, Any]]:
    samples = []
    stale_count = 0
    last_stale_uptime = None
    other_methods = set()
    deadline = time.monotonic() + timeout
    started = time.monotonic()
    next_progress = started + 30
    while len(samples) < count:
        if refresh is not None:
            refresh()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(
                f"timed out waiting for {count} runtime_info heap samples after uptime {after_uptime}; "
                f"received {len(samples)}, discarded {stale_count} stale samples "
                f"(last uptime {last_stale_uptime}), other methods {sorted(other_methods)}"
            )
        payload = json.loads(recv_ws_text(sock, timeout=remaining))
        if payload.get("method") != "runtime_info":
            other_methods.add(str(payload.get("method", "<no method>")))
            continue
        runtime = payload.get("params", {})
        heap = runtime.get("heap_free")
        if not isinstance(heap, int) or isinstance(heap, bool) or heap <= 0:
            raise AssertionError(f"invalid runtime heap measurement: {runtime}")
        if runtime.get("uptime", -1) < after_uptime:
            stale_count += 1
            last_stale_uptime = runtime.get("uptime")
            continue
        samples.append({"received_at": time.time(), **runtime})
        now = time.monotonic()
        if now >= next_progress:
            print(f"runtime_info: received {len(samples)}/{count} fresh samples after {now - started:.0f}s",
                  flush=True)
            next_progress = now + 30
    print(f"runtime_info: collected {count} fresh samples in {time.monotonic() - started:.1f}s", flush=True)
    return samples


def subscribe_runtime_info(sock: socket.socket) -> None:
    response, _ = ws_request(sock, {"jsonrpc": "2.0", "id": 9801,
                                    "method": "runtime_info_subscribe", "params": {}})
    assert response.get("result", {}).get("subscribed") is True


def heap_stage(phase: str, count: int, samples: list[dict[str, Any]], baseline: float, previous: dict[str, Any] | None) -> dict[str, Any]:
    heaps = [sample["heap_free"] for sample in samples]
    median = statistics.median(heaps)
    added = count - previous["advertised_count"] if previous else 0
    return {
        "phase": phase,
        "advertised_count": count,
        "heap_min": min(heaps),
        "heap_max": max(heaps),
        "heap_median": median,
        "drop_from_baseline": baseline - median,
        "incremental_bytes_per_controller": (previous["heap_median"] - median) / added if added > 0 else None,
        "samples": samples,
    }


@pytest.mark.parametrize("index", [0, 1, 31, 255])
def test_virtual_controller_records(index: int) -> None:
    address = controller_ip("192.0.2.1", index)
    controller = VirtualController(index, address, is_leader=True)
    services = controller.build_services()
    assert {service.type for service in services} == {
        "_lightinator-api._tcp.local.", "_lightinator._tcp.local.", "_http._tcp.local."
    }
    for service in services:
        assert service.parsed_addresses() == [address]
        assert service.server == controller.hostname_fqdn
        assert service.port == 80
        assert service.properties[b"id"] == str(controller.chip_id).encode()
        assert service.properties[b"host_type"] == b"CONTROLLER"
        assert b"webapp" not in service.properties
    assert services[1].properties[b"isLeader"] == b"1"


@pytest.mark.parametrize("start,index", [("255.255.255.255", 1), ("192.0.2.1", -1), ("not-an-ip", 0)])
def test_virtual_ip_rejects_invalid_ranges(start: str, index: int) -> None:
    with pytest.raises(ValueError):
        controller_ip(start, index)


@pytest.mark.parametrize("value", ["", "4,8", "0", "0,4,4", "0,-1", "0,8,4"])
def test_invalid_ramp(value: str) -> None:
    with pytest.raises(ValueError):
        ramp_counts(value)


def test_valid_ramp() -> None:
    assert ramp_counts("0,4,8,16,32") == [0, 4, 8, 16, 32]


def example_hosts(controller: VirtualController, visible: bool = True) -> dict[str, Any]:
    return {"hosts": [{"id": controller.chip_id, "hostname": controller.hostname_raw,
                       "ip_address": controller.ip_address, "host_type": "CONTROLLER",
                       "visible": visible, "state": 3 if visible else 2}]}


@pytest.mark.parametrize("field,value", [("id", 42), ("hostname", "wrong"), ("ip_address", "192.0.2.2"),
                                         ("host_type", "UNKNOWN"), ("visible", False), ("state", "ONLINE")])
def test_hosts_detects_wrong_records(field: str, value: Any) -> None:
    controller = VirtualController(0, "192.0.2.1")
    payload = example_hosts(controller)
    payload["hosts"][0][field] = value
    with pytest.raises(AssertionError):
        check_hosts(payload, [controller], {controller.chip_id}, all_hosts=True)


def test_hosts_detects_duplicates() -> None:
    controller = VirtualController(0, "192.0.2.1")
    payload = example_hosts(controller)
    payload["hosts"].append(dict(payload["hosts"][0]))
    with pytest.raises(AssertionError, match="duplicate"):
        check_hosts(payload, [controller], {controller.chip_id}, all_hosts=True)


def test_hosts_withdrawal_retains_offline_inventory() -> None:
    controller = VirtualController(0, "192.0.2.1")
    check_hosts(example_hosts(controller, False), [controller], set(), all_hosts=True)
    check_hosts({"hosts": []}, [controller], set(), all_hosts=False)


def test_heap_stage_reports_cost_without_assuming_a_budget() -> None:
    baseline = heap_stage("ramp", 0, [{"heap_free": 24000}], 24000, None)
    stage = heap_stage("ramp", 4, [{"heap_free": 23000}, {"heap_free": 23200}, {"heap_free": 23100}], 24000, baseline)
    assert stage["heap_median"] == 23100
    assert stage["drop_from_baseline"] == 900
    assert stage["incremental_bytes_per_controller"] == 225


@pytest.mark.parametrize("heap", [0, -1, "24000", True])
def test_runtime_rejects_invalid_heap(monkeypatch: pytest.MonkeyPatch, heap: Any) -> None:
    monkeypatch.setattr(sys.modules[__name__], "recv_ws_text", lambda *args, **kwargs: json.dumps({"method": "runtime_info", "params": {"heap_free": heap}}))
    with pytest.raises(AssertionError, match="heap measurement"):
        runtime_samples(None, 1, 1)


def test_runtime_discards_queued_and_unrelated_notifications(monkeypatch: pytest.MonkeyPatch) -> None:
    frames = iter([{"method": "color_event", "params": {}},
                   {"method": "runtime_info", "params": {"heap_free": 25000, "uptime": 10}},
                   {"method": "runtime_info", "params": {"heap_free": 24000, "uptime": 30}}])
    monkeypatch.setattr(sys.modules[__name__], "recv_ws_text", lambda *args, **kwargs: json.dumps(next(frames)))
    assert runtime_samples(None, 1, 1, after_uptime=20)[0]["heap_free"] == 24000


def test_discovery_timeout_reports_last_mismatch(monkeypatch: pytest.MonkeyPatch) -> None:
    clock = iter([0, 0, 2])
    monkeypatch.setattr(time, "monotonic", lambda: next(clock))
    monkeypatch.setattr(time, "sleep", lambda *args: None)
    monkeypatch.setattr(sys.modules[__name__], "read_json", lambda url: {"hosts": []})
    with pytest.raises(AssertionError, match="mDNS did not converge"):
        wait_for_hosts("http://unused", [VirtualController(0, "192.0.2.1")], {BASE_CHIP_ID}, 1)


def test_virtual_controller_identity_change_keeps_id() -> None:
    controller = VirtualController(3, "192.0.2.3")
    chip_id = controller.chip_id
    controller.set_hostname("renamed-ctrl")
    controller.set_ip("192.0.2.200")
    services = controller.build_services()
    assert controller.chip_id == chip_id
    for service in services:
        assert service.server == "renamed-ctrl.local."
        assert service.parsed_addresses() == ["192.0.2.200"]
        assert service.properties[b"id"] == str(chip_id).encode()


def test_change_counters_require_firmware_support() -> None:
    with pytest.raises(AssertionError, match="mdns_hostname_changes"):
        change_counters_from_info({"debug": {"mdns_ip_changes": 0}})
    assert change_counters_from_info({"debug": {"mdns_hostname_changes": 2, "mdns_ip_changes": 5}}) == (2, 5)


@pytest.mark.parametrize("ttl", [30, 60])
def test_settling_waits_two_ttls_and_refreshes(monkeypatch: pytest.MonkeyPatch, ttl: int) -> None:
    clock = [0.0]
    refreshes = []
    monkeypatch.setattr(time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(time, "sleep", lambda seconds: clock.__setitem__(0, clock[0] + seconds))
    settle_for_ttl(ttl, lambda: refreshes.append(clock[0]))
    assert clock[0] == 2 * ttl
    assert refreshes and max(refreshes) < 2 * ttl


@pytest.mark.parametrize("fail_sampling", [False, True])
def test_ramp_sequence_and_cleanup_offline(monkeypatch: pytest.MonkeyPatch, tmp_path: Path, fail_sampling: bool) -> None:
    module = sys.modules[__name__]
    monkeypatch.setenv("HOST_MDNS_INTERFACE_IP", "192.0.2.10")
    monkeypatch.setenv("HOST_MDNS_IP_START", "192.0.2.100")
    monkeypatch.setenv("HOST_MDNS_COUNTS", "0,2,4")
    monkeypatch.setenv("HOST_MDNS_HEAP_SAMPLES", "1")
    monkeypatch.setenv("HOST_MDNS_TTL", "30")
    monkeypatch.setenv("HOST_SMOKE_REAL_DEVICE", "0")

    class FakeSocket:
        closed = False

        def close(self):
            self.closed = True

    class FakeZeroconf:
        def __init__(self):
            self.services = []
            self.inventory = {}
            self.closed = False

        def register_service(self, service, ttl):
            assert ttl == service.host_ttl == service.other_ttl == 30
            self.services.append(service)
            identity = int(service.properties[b"id"])
            self.inventory[identity] = {"id": identity, "hostname": service.server,
                                        "ip_address": service.parsed_addresses()[0],
                                        "host_type": "CONTROLLER", "visible": True, "state": 3}

        def update_service(self, service):
            assert service in self.services

        def unregister_service(self, service):
            self.services.remove(service)
            identity = int(service.properties[b"id"])
            if not any(int(item.properties[b"id"]) == identity for item in self.services):
                self.inventory[identity].update(visible=False, state=2)

        def close(self):
            self.closed = True

    sock = FakeSocket()
    fleet = FakeZeroconf()
    monkeypatch.setattr(zeroconf, "Zeroconf", lambda **kwargs: fleet)
    monkeypatch.setattr(module, "websocket_handshake", lambda *args: (sock, {"response": {"status_line": "101"}}))
    monkeypatch.setattr(module, "ws_request", lambda *args: ({"result": {"subscribed": True}}, {}))

    def fake_read(url):
        if "/info" in url:
            return {"runtime": {"uptime": 100}}
        return {"hosts": [dict(row) for row in fleet.inventory.values() if row["visible"] or "all=true" in url]}

    def fake_samples(*args, **kwargs):
        if fail_sampling and fleet.services:
            raise AssertionError("simulated measurement failure")
        return [{"heap_free": 24000 - len(fleet.inventory) * 200, "uptime": 110}]

    monkeypatch.setattr(module, "read_json", fake_read)
    monkeypatch.setattr(module, "runtime_samples", fake_samples)
    settled_counts = []
    monkeypatch.setattr(module, "settle_for_ttl", lambda ttl, refresh=None: settled_counts.append(len(fleet.services) // 3))
    config = SmokeConfig("192.0.2.2", "http://unused", "http://unused/info", "http://unused/color",
                         "192.0.2.2", 80, "/ws", "", 4821, tmp_path,
                         tmp_path / "http.jsonl", tmp_path / "malformed.json")
    if fail_sampling:
        with pytest.raises(AssertionError, match="simulated measurement failure"):
            test_visible_controller_heap_ramp(config)
    else:
        test_visible_controller_heap_ramp(config)
        stages = [json.loads(line) for line in (tmp_path / "mdns-heap-ramp.jsonl").read_text().splitlines()]
        assert [row["advertised_count"] for row in stages] == [0, 2, 4, 0]
        assert stages[-1]["phase"] == "withdrawn"
        assert stages[1]["incremental_bytes_per_controller"] == 200
        assert stages[-1]["drop_from_baseline"] == 800
        assert settled_counts == [0, 2, 4, 0]
    assert sock.closed and fleet.closed and not fleet.services


@pytest.mark.skipif(os.environ.get("HOST_MDNS_RAMP") != "1", reason="opt-in TAP mDNS/RAM integration test")
def test_visible_controller_heap_ramp(smoke_config: SmokeConfig) -> None:
    if os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1":
        pytest.skip("mDNS ramp persists inventory; use an isolated Host emulator")
    interface_ip = os.environ["HOST_MDNS_INTERFACE_IP"]
    ip_start = os.environ.get("HOST_MDNS_IP_START", "192.168.13.100")
    counts = ramp_counts(os.environ.get("HOST_MDNS_COUNTS", "0,4,8,16,32,64,96,128"))
    base_id = int(os.environ.get("HOST_MDNS_BASE_ID", str(BASE_CHIP_ID)))
    timeout = float(os.environ.get("HOST_MDNS_TIMEOUT", "300"))
    sample_count = int(os.environ.get("HOST_MDNS_HEAP_SAMPLES", "3"))
    ttl = int(os.environ.get("HOST_MDNS_TTL", "30"))
    assert ttl > 0
    assert sample_count > 0
    assert counts[-1] <= 128, "use at most 128 synthetic controllers per isolated run"
    fleet = [VirtualController(index, controller_ip(ip_start, index), base_chip_id=base_id)
             for index in range(counts[-1])]
    assert smoke_config.app_ip not in {controller.ip_address for controller in fleet}
    assert interface_ip not in {controller.ip_address for controller in fleet}
    initial_hosts = read_json(f"{smoke_config.base_url}/hosts?all=true")
    write_json(smoke_config.log_dir / "mdns-initial-hosts.json", initial_hosts)
    existing = {int(row["id"]) for row in host_records(initial_hosts)}
    assert not existing.intersection(controller.chip_id for controller in fleet), "use fresh Host storage or a different HOST_MDNS_BASE_ID"

    report = smoke_config.log_dir / "mdns-heap-ramp.jsonl"
    report.write_text("", encoding="utf-8")
    sock, trace = websocket_handshake(smoke_config, os.urandom(16))
    zc = None
    registered = []
    stages = []
    last_refresh = 0.0

    def refresh_services() -> None:
        nonlocal last_refresh
        now = time.monotonic()
        if not registered or now - last_refresh < ttl / 3:
            return
        last_refresh = now
        for service in registered:
            zc.update_service(service)

    try:
        assert "101" in trace["response"]["status_line"]
        subscribe_runtime_info(sock)
        print(f"Settling baseline for {2 * ttl}s", flush=True)
        settle_for_ttl(ttl)
        subscribe_runtime_info(sock)
        stage_uptime = read_json(f"{smoke_config.base_url}/info?v=2&sparse=false")["runtime"]["uptime"]
        baseline_samples = runtime_samples(sock, sample_count, timeout, after_uptime=stage_uptime + 10)
        baseline = statistics.median(sample["heap_free"] for sample in baseline_samples)
        stages.append(heap_stage("ramp", 0, baseline_samples, baseline, None))
        stages[-1].update(ttl_seconds=ttl, settle_seconds=2 * ttl)
        append_jsonl(report, stages[-1])
        zc = zeroconf.Zeroconf(interfaces=[interface_ip], ip_version=zeroconf.IPVersion.V4Only)
        for count in counts[1:]:
            for controller in fleet[len(registered) // 3:count]:
                for service in controller.build_services():
                    refresh_services()
                    service.host_ttl = ttl
                    service.other_ttl = ttl
                    zc.register_service(service, ttl=ttl)
                    registered.append(service)
            advertised = fleet[:count]
            visible = {controller.chip_id for controller in advertised}
            print(f"Settling {count} advertised controllers for {2 * ttl}s", flush=True)
            settle_for_ttl(ttl, refresh_services)
            wait_for_hosts(smoke_config.base_url, advertised, visible, timeout, refresh_services,
                           label=f"{count}-controller pre-measurement inventory")
            subscribe_runtime_info(sock)
            stage_uptime = read_json(f"{smoke_config.base_url}/info?v=2&sparse=false")["runtime"]["uptime"]
            samples = runtime_samples(sock, sample_count, timeout, after_uptime=stage_uptime + 10, refresh=refresh_services)
            wait_for_hosts(smoke_config.base_url, advertised, visible, timeout, refresh_services,
                           label=f"{count}-controller post-measurement inventory")
            assert min(sample["uptime"] for sample in samples) >= stages[-1]["samples"][-1]["uptime"], "emulator restarted"
            stages.append(heap_stage("ramp", count, samples, baseline, stages[-1]))
            stages[-1].update(ttl_seconds=ttl, settle_seconds=2 * ttl)
            append_jsonl(report, stages[-1])
        for service in registered:
            zc.unregister_service(service)
        registered.clear()
        print(f"Settling withdrawal for {2 * ttl}s", flush=True)
        settle_for_ttl(ttl)
        wait_for_hosts(smoke_config.base_url, fleet, set(), timeout, label="withdrawn inventory")
        subscribe_runtime_info(sock)
        stage_uptime = read_json(f"{smoke_config.base_url}/info?v=2&sparse=false")["runtime"]["uptime"]
        samples = runtime_samples(sock, sample_count, timeout, after_uptime=stage_uptime + 10)
        stages.append(heap_stage("withdrawn", 0, samples, baseline, stages[-1]))
        stages[-1].update(ttl_seconds=ttl, settle_seconds=2 * ttl)
        append_jsonl(report, stages[-1])
        print(f"mDNS ramp report: {report}")
        for stage in stages:
            print(f"{stage['phase']} count={stage['advertised_count']} heap={stage['heap_median']} drop={stage['drop_from_baseline']} cost={stage['incremental_bytes_per_controller']}")
    finally:
        try:
            if zc is not None:
                try:
                    for service in registered:
                        zc.unregister_service(service)
                finally:
                    zc.close()
        finally:
            sock.close()