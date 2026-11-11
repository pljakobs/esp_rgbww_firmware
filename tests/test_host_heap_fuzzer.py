"""Deterministic Host-only HTTP API stress sweep.

Run against a debug Host emulator with HOST_SMOKE_APP_IP and
HOST_SMOKE_LOG_DIR set. Results are written to host-api-heap-fuzzer.json.
"""

from __future__ import annotations

import json
import os
from collections import Counter
from collections.abc import Callable, Iterator
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

import pytest

from host_smoke_api_test import (
    HostHeapControl,
    SmokeConfig,
    api_secured,
    http_request,
    smoke_config,
    write_json,
)


HTTP_PATHS = (
    "/", "/webapp", "/config", "/info", "/color", "/networks", "/scan_networks",
    "/webapp_status", "/webapp_check", "/system", "/update", "/connect", "/ping",
    "/hosts", "/data", "/on", "/off", "/stop", "/skip", "/pause",
    "/continue", "/blink", "/toggle", "/canonical.html", "/generate_204",
    "/static/hotspot.txt", "/connecttest.txt", "/hotspot-detect.html",
    "/nmcheck.gnome.org", "/ws",
)
PAGE_LOAD_BURST = ("/", "/info", "/config", "/data", "/hosts", "/color")
SWEEPS_PER_STEP = 10

# Avoid starting OTA, Wi-Fi scans, or connection attempts. Invalid bodies exercise
# parsing/rejection paths for command endpoints without applying those commands.
SAFE_POSTS = {
    "/config": b"{}",
    "/color": b"{",
    "/system": b'{"cmd":"__heap_fuzz_unknown_command__"}',
    "/update": b"{}",
    "/connect": b'{"password":"fuzz"}',
    "/on": b"{",
    "/off": b"{",
    "/stop": b"{",
    "/skip": b"{",
    "/pause": b"{",
    "/continue": b"{",
    "/blink": b"{",
    "/toggle": b"{",
}


def _heap_command(control: HostHeapControl, payload: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any] | None]:
    try:
        result = control.command(payload)
    except Exception as exc:  # noqa: BLE001
        return {"status": 0, "error": repr(exc)}, None
    return {"status": 200, "error": ""}, result


def _sample_heap(control: HostHeapControl, report: dict[str, Any], label: str) -> dict[str, Any] | None:
    response, body = _heap_command(control, {"cmd": "get-free"})
    report["heap_samples"].append({"label": label, "response": response, "heap": body})
    if response["status"] != 200 or not body or body.get("success") is not True:
        return None
    return body


def _allocate_to_target(
    control: HostHeapControl,
    report: dict[str, Any],
    held: list[int],
    target_free: int,
    profile: str,
    after_allocation: Callable[[], bool],
) -> None:
    while len(held) < 32:
        before = _sample_heap(control, report, f"{profile}-alloc-{len(held)}")
        if before is None or int(before["free"]) <= target_free:
            return
        response, result = _heap_command(control, {"cmd": "alloc", "size": 512})
        report["allocations"].append({"profile": profile, "size": 512, "response": response, "result": result})
        if response["status"] != 200 or not result or result.get("success") is not True:
            return
        held.append(int(result["address"]))
        if not after_allocation():
            return


def _free_address(control: HostHeapControl, report: dict[str, Any], address: int, label: str) -> bool:
    response, result = _heap_command(control, {"cmd": "free", "address": address})
    report["frees"].append({"label": label, "address": address, "response": response, "result": result})
    return response["status"] == 200 and result is not None and result.get("success") is True


def _api_sweep(smoke_config: SmokeConfig) -> Iterator[dict[str, Any]]:
    for path in HTTP_PATHS:
        for method in ("GET", "OPTIONS"):
            response = http_request(method, f"{smoke_config.base_url}{path}", timeout=4)
            yield {"path": path, "method": method, "response": response}

        post_body = SAFE_POSTS.get(path)
        if post_body is not None:
            response = http_request(
                "POST",
                f"{smoke_config.base_url}{path}",
                body=post_body,
                headers={"Content-Type": "application/json"},
                timeout=4,
            )
            yield {
                "path": path,
                "method": "POST",
                "body": post_body.decode("utf-8", errors="replace"),
                "response": response,
            }

    with ThreadPoolExecutor(max_workers=3) as executor:
        futures = {
            path: executor.submit(http_request, "GET", f"{smoke_config.base_url}{path}", timeout=8)
            for path in PAGE_LOAD_BURST
        }
        for path, future in futures.items():
            yield {"path": path, "method": "GET", "burst": True, "response": future.result(timeout=10)}


def test_host_api_heap_fuzzer(
    smoke_config: SmokeConfig, api_secured: bool, request: pytest.FixtureRequest
) -> None:
    if os.environ.get("HOST_SMOKE_REAL_DEVICE") == "1":
        pytest.skip("heap fuzzer is restricted to debug Host builds")
    if api_secured:
        pytest.skip("heap fuzzer currently requires an unsecured Host controller")

    report: dict[str, Any] = {
        "host": smoke_config.app_ip,
        "sweeps_per_step": SWEEPS_PER_STEP,
        "profiles": [],
        "heap_samples": [],
        "allocations": [],
        "frees": [],
        "probes": [],
    }
    report_path = smoke_config.log_dir / "host-api-heap-fuzzer.json"
    baseline_failures: list[dict[str, Any]] = []
    pressured_transport_errors: Counter[str] = Counter()
    try:
        control = HostHeapControl(smoke_config)
    except Exception as exc:  # noqa: BLE001
        pytest.fail(f"Cannot open Host heap-control WebSocket: {exc}")
    request.addfinalizer(control.close)

    baseline = _sample_heap(control, report, "baseline")
    if baseline is None:
        control.close()
        pytest.fail("Host heap_test WebSocket method unavailable; build and run a debug Host firmware")

    baseline_free = int(baseline["free"])
    profiles = (
        ("baseline", baseline_free, False),
        ("constrained", max(12500, baseline_free * 70 // 100), False),
        ("fragmented", max(11500, baseline_free * 60 // 100), True),
    )

    for profile, target_free, fragment in profiles:
        held: list[int] = []
        try:
            start_heap = _sample_heap(control, report, f"{profile}-start")
            profile_result = {
                "name": profile,
                "target_free": target_free,
                "start_heap": start_heap,
                "held_blocks": len(held),
                "fragmented_by_alternating_frees": fragment,
                "transport_errors": 0,
                "status_counts": {},
                "steps": [],
            }
            report["profiles"].append(profile_result)

            def run_sweeps(stage: str) -> bool:
                step = {
                    "stage": stage,
                    "held_blocks": len(held),
                    "iterations": 0,
                    "start_heap": _sample_heap(control, report, f"{profile}-{stage}-{len(held)}-start"),
                }
                profile_result["steps"].append(step)
                profile_result["held_blocks"] = len(held)
                for iteration in range(1, SWEEPS_PER_STEP + 1):
                    for probe in _api_sweep(smoke_config):
                        probe.update({
                            "profile": profile,
                            "stage": stage,
                            "allocation_step": len(held),
                            "iteration": iteration,
                        })
                        report["probes"].append(probe)
                        response = probe["response"]
                        status = str(response["status"])
                        profile_result["status_counts"][status] = profile_result["status_counts"].get(status, 0) + 1
                        if response["error"]:
                            profile_result["transport_errors"] += 1
                            if profile == "baseline":
                                baseline_failures.append(probe)
                            else:
                                pressured_transport_errors[profile] += 1
                    step["iterations"] = iteration
                    end_heap = _sample_heap(control, report, f"{profile}-{stage}-{len(held)}-run-{iteration}")
                    step["end_heap"] = end_heap
                    profile_result["end_heap"] = end_heap
                    if end_heap is None:
                        return False
                print(json.dumps({"profile": profile, "step": step}, sort_keys=True), flush=True)
                return True

            if profile != "baseline":
                _allocate_to_target(
                    control, report, held, target_free, profile, lambda: run_sweeps("allocation")
                )
            if not profile_result["steps"]:
                run_sweeps("baseline" if profile == "baseline" else "target")
            if fragment and profile_result.get("end_heap") is not None:
                survivors: list[int] = []
                for index, address in enumerate(held):
                    if index % 2 == 0:
                        if not _free_address(control, report, address, f"{profile}-alternating-hole"):
                            survivors.append(address)
                    else:
                        survivors.append(address)
                held = survivors
                run_sweeps("alternating-frees")

            if profile_result.get("end_heap") is None:
                failure = {"profile": profile, "error": "controller stopped answering heap_test"}
                if profile == "baseline":
                    baseline_failures.append(failure)
                else:
                    pressured_transport_errors[profile] += 1
                break
        finally:
            cleanup_failures = []
            for address in reversed(held):
                if not _free_address(control, report, address, f"{profile}-cleanup"):
                    cleanup_failures.append(address)
            if cleanup_failures:
                report.setdefault("cleanup_failures", {})[profile] = cleanup_failures

    final_heap = _sample_heap(control, report, "final")
    control.close()
    report["final_heap"] = final_heap
    write_json(report_path, report)

    print(json.dumps({
        "report": str(report_path),
        "profiles": [
            {
                "name": item["name"],
                "target_free": item["target_free"],
                "start_free": item["start_heap"].get("free") if item["start_heap"] else None,
                "end_free": item.get("end_heap", {}).get("free") if item.get("end_heap") else None,
                "held_blocks": item["held_blocks"],
                "steps": len(item["steps"]),
                "sweeps": sum(step["iterations"] for step in item["steps"]),
                "transport_errors": item["transport_errors"],
                "status_counts": item["status_counts"],
            }
            for item in report["profiles"]
        ],
        "pressured_transport_errors": dict(pressured_transport_errors),
        "cleanup_failures": report.get("cleanup_failures", {}),
        "final_heap": final_heap,
    }, sort_keys=True))

    if baseline_failures:
        pytest.fail(
            f"Heap fuzzer saw {len(baseline_failures)} baseline transport failures; report: {report_path}\n"
            + json.dumps(baseline_failures[:12], ensure_ascii=True, sort_keys=True)
        )
    if report.get("cleanup_failures"):
        pytest.fail(f"Heap fuzzer could not free all blocks: {report['cleanup_failures']}; report: {report_path}")
    if final_heap is None or int(final_heap["blocks"]) != int(baseline["blocks"]):
        pytest.fail(f"Heap fuzzer did not restore allocation baseline; report: {report_path}")
