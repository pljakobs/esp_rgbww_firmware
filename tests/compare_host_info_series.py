#!/usr/bin/env python3
"""Run Host binaries under valgrind, sample /info, and write one combined report.

This script starts each app binary (testing, then feature), optionally under
valgrind, polls /info in fixed intervals, stops the app, and writes a single
JSON report containing runtime samples plus parsed valgrind statistics.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any
from urllib.request import urlopen


VALGRIND_SUMMARY_PATTERNS = {
    "total_heap_usage": re.compile(
        r"total heap usage:\s*(?P<allocs>[\d,]+) allocs,\s*(?P<frees>[\d,]+) frees,\s*(?P<bytes>[\d,]+) bytes allocated"
    ),
    "in_use_at_exit": re.compile(
        r"in use at exit:\s*(?P<bytes>[\d,]+) bytes in\s*(?P<blocks>[\d,]+) blocks"
    ),
    "definitely_lost": re.compile(
        r"definitely lost:\s*(?P<bytes>[\d,]+) bytes in\s*(?P<blocks>[\d,]+) blocks"
    ),
    "indirectly_lost": re.compile(
        r"indirectly lost:\s*(?P<bytes>[\d,]+) bytes in\s*(?P<blocks>[\d,]+) blocks"
    ),
    "possibly_lost": re.compile(
        r"possibly lost:\s*(?P<bytes>[\d,]+) bytes in\s*(?P<blocks>[\d,]+) blocks"
    ),
    "still_reachable": re.compile(
        r"still reachable:\s*(?P<bytes>[\d,]+) bytes in\s*(?P<blocks>[\d,]+) blocks"
    ),
    "suppressed": re.compile(
        r"suppressed:\s*(?P<bytes>[\d,]+) bytes in\s*(?P<blocks>[\d,]+) blocks"
    ),
    "error_summary": re.compile(r"ERROR SUMMARY:\s*(?P<errors>[\d,]+) errors from\s*(?P<contexts>[\d,]+) contexts"),
}

DEFAULT_VALGRIND_ARGS = [
    "--num-callers=40",
    "--read-var-info=yes",
    "--keep-debuginfo=yes",
]


def fetch_info(url: str, timeout: float) -> tuple[bool, dict[str, Any]]:
    try:
        with urlopen(url, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
        data = json.loads(raw)
        if isinstance(data, dict):
            return True, data
    except Exception:
        pass
    return False, {}


def pick_field(payload: dict[str, Any], *keys: str) -> Any:
    if not isinstance(payload, dict):
        return None

    lowered = {str(k).lower(): v for k, v in payload.items()}
    for key in keys:
        if key in payload:
            return payload[key]
        value = lowered.get(key.lower())
        if value is not None:
            return value
    return None


def runtime_fields(payload: dict[str, Any]) -> dict[str, Any]:
    runtime = payload.get("runtime", {}) if isinstance(payload, dict) else {}
    return {
        "heap_free": pick_field(runtime, "heap_free", "heapFree")
        or pick_field(payload, "heap_free", "heapFree"),
        "minimumfreeHeapRuntime": pick_field(runtime, "minimumfreeHeapRuntime")
        or pick_field(payload, "minimumfreeHeapRuntime"),
        "minimumfreeHeap10min": pick_field(runtime, "minimumfreeHeap10min")
        or pick_field(payload, "minimumfreeHeap10min"),
    }


def parse_int(value: str) -> int:
    return int(value.replace(",", ""))


def parse_valgrind_log(log_path: Path) -> dict[str, Any]:
    if not log_path.exists():
        return {"log": str(log_path), "present": False}

    text = log_path.read_text(encoding="utf-8", errors="replace")
    summary: dict[str, Any] = {
        "log": str(log_path),
        "present": True,
    }

    for key, pattern in VALGRIND_SUMMARY_PATTERNS.items():
        match = pattern.search(text)
        if not match:
            summary[key] = None
            continue

        groups = {name: parse_int(value) for name, value in match.groupdict().items()}
        summary[key] = groups

    return summary


def summarize_xtree(xtree_path: Path, top_n: int) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "path": str(xtree_path),
        "present": xtree_path.exists(),
        "annotator": "callgrind_annotate",
        "available": False,
        "top_lines": [],
    }

    if not xtree_path.exists():
        return summary

    annotator = shutil.which("callgrind_annotate")
    if not annotator:
        return summary

    summary["available"] = True

    try:
        result = subprocess.run(
            [annotator, "--auto=yes", "--inclusive=yes", str(xtree_path)],
            check=False,
            capture_output=True,
            text=True,
        )
    except Exception as exc:
        summary["error"] = str(exc)
        return summary

    summary["exit_code"] = result.returncode
    if result.returncode != 0:
        summary["stderr"] = result.stderr[-4000:]
        return summary

    top_lines: list[str] = []
    capture = False
    for raw_line in result.stdout.splitlines():
        line = raw_line.rstrip()
        if not capture and ("Ir " in line or "PROGRAM TOTALS" in line):
            capture = True
        if not capture:
            continue
        if line:
            top_lines.append(line)
        if len(top_lines) >= top_n:
            break

    summary["top_lines"] = top_lines
    return summary


def stop_process_group(proc: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGINT)
    except Exception:
        return

    try:
        proc.wait(timeout=5)
    except Exception:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            pass


def stop_stale_processes(app_path: str) -> None:
    """Stop pre-existing app processes without killing this Python script.

    The comparison script may receive app paths as command-line arguments, so a
    broad `pkill -f <app_path>` can match and terminate the script itself.
    """

    current_pid = os.getpid()
    try:
        result = subprocess.run(
            ["pgrep", "-af", app_path],
            check=False,
            capture_output=True,
            text=True,
        )
    except Exception:
        return

    for line in result.stdout.splitlines():
        parts = line.strip().split(maxsplit=1)
        if not parts:
            continue

        try:
            pid = int(parts[0])
        except ValueError:
            continue

        command = parts[1] if len(parts) > 1 else ""
        if pid == current_pid:
            continue
        if "compare_host_info_series.py" in command:
            continue

        try:
            os.kill(pid, signal.SIGINT)
        except ProcessLookupError:
            continue
        except Exception:
            try:
                os.kill(pid, signal.SIGKILL)
            except Exception:
                pass


def run_capture(
    label: str,
    app_path: str,
    info_url: str,
    startup_wait_seconds: int,
    sample_count: int,
    interval_seconds: int,
    request_timeout_seconds: float,
    output_dir: Path,
    valgrind_enabled: bool,
    valgrind_tool: str,
    valgrind_args: list[str],
    xtree_top_n: int,
) -> dict[str, Any]:
    output_dir.mkdir(parents=True, exist_ok=True)

    jsonl_path = output_dir / f"{label}_info_samples.jsonl"
    runlog_path = output_dir / f"{label}_runtime.log"
    valgrind_log_path = output_dir / f"{label}_valgrind.log"
    valgrind_xtree_path = output_dir / f"{label}_valgrind.xtree"

    # Ensure no stale instance is still running.
    stop_stale_processes(app_path)

    command = [app_path]
    if valgrind_enabled:
        command = [
            valgrind_tool,
            "--tool=memcheck",
            "--leak-check=full",
            "--show-leak-kinds=all",
            "--xtree-memory=full",
            f"--xtree-memory-file={valgrind_xtree_path}",
            f"--log-file={valgrind_log_path}",
            *DEFAULT_VALGRIND_ARGS,
            *valgrind_args,
            app_path,
        ]

    with open(runlog_path, "wb") as runlog:
        proc = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=runlog,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

    rows: list[dict[str, Any]] = []

    try:
        start = time.time()
        while time.time() - start < startup_wait_seconds:
            ok, _ = fetch_info(info_url, timeout=request_timeout_seconds)
            if ok:
                break
            time.sleep(1)

        for sample in range(sample_count):
            ts = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            ok, data = fetch_info(info_url, timeout=request_timeout_seconds)
            rows.append(
                {
                    "ts": ts,
                    "label": label,
                    "sample": sample,
                    "ok": ok,
                    "data": data if ok else {},
                }
            )
            if sample < sample_count - 1:
                time.sleep(interval_seconds)
    finally:
        stop_process_group(proc)

    with open(jsonl_path, "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=True) + "\n")

    values = [runtime_fields(r["data"]) for r in rows if r["ok"]]
    heap = [v["heap_free"] for v in values if isinstance(v.get("heap_free"), (int, float))]
    min_runtime = [
        v["minimumfreeHeapRuntime"]
        for v in values
        if isinstance(v.get("minimumfreeHeapRuntime"), (int, float))
    ]
    min_10min = [
        v["minimumfreeHeap10min"]
        for v in values
        if isinstance(v.get("minimumfreeHeap10min"), (int, float))
    ]
    valgrind_summary = parse_valgrind_log(valgrind_log_path) if valgrind_enabled else None
    xtree_summary = summarize_xtree(valgrind_xtree_path, xtree_top_n) if valgrind_enabled else None

    return {
        "label": label,
        "samples": len(rows),
        "ok_samples": sum(1 for r in rows if r["ok"]),
        "heap_free_values": heap,
        "minimumfreeHeapRuntime_values": min_runtime,
        "minimumfreeHeap10min_values": min_10min,
        "heap_free_min": min(heap) if heap else None,
        "heap_free_median": statistics.median(heap) if heap else None,
        "minimumfreeHeapRuntime_min": min(min_runtime) if min_runtime else None,
        "minimumfreeHeap10min_min": min(min_10min) if min_10min else None,
        "jsonl": str(jsonl_path),
        "runlog": str(runlog_path),
        "valgrind": valgrind_summary,
        "valgrind_xtree": str(valgrind_xtree_path) if valgrind_enabled else None,
        "valgrind_xtree_summary": xtree_summary,
    }


def default_paths() -> tuple[str, str]:
    testing = "/home/pjakobs/devel/esp_rgbww_firmware_wt_testing/out/Host/debug/firmware/app"
    feature = "/home/pjakobs/devel/esp_rgbww_firmware_wt_feat/out/Host/debug/firmware/app"
    return testing, feature


def parse_args() -> argparse.Namespace:
    testing_default, feature_default = default_paths()

    parser = argparse.ArgumentParser(description="Compare runtime /info heap metrics for two Host binaries.")
    parser.add_argument("--testing-app", default=testing_default, help="Path to testing Host app binary.")
    parser.add_argument("--feature-app", default=feature_default, help="Path to feature Host app binary.")
    parser.add_argument("--info-url", default="http://192.168.13.10/info?v=2", help="URL to poll.")
    parser.add_argument("--startup-wait", type=int, default=20, help="Startup wait in seconds.")
    parser.add_argument("--samples", type=int, default=6, help="Number of samples per app.")
    parser.add_argument("--interval", type=int, default=10, help="Seconds between samples.")
    parser.add_argument("--request-timeout", type=float, default=4.0, help="HTTP timeout per sample.")
    parser.add_argument(
        "--no-valgrind",
        action="store_true",
        help="Disable valgrind and only collect /info samples.",
    )
    parser.add_argument(
        "--valgrind-tool",
        default="valgrind",
        help="Valgrind executable to use.",
    )
    parser.add_argument(
        "--valgrind-arg",
        action="append",
        default=[],
        help="Additional argument passed through to valgrind. Can be repeated.",
    )
    parser.add_argument(
        "--xtree-top",
        type=int,
        default=25,
        help="Number of annotated xtree lines to embed in the combined report.",
    )
    parser.add_argument(
        "--output-dir",
        default="out/host-ci/info-series",
        help="Directory for logs, xtree files, and the combined report json.",
    )
    return parser.parse_args()


def compute_delta(summary: dict[str, Any], key: str) -> float | None:
    t = summary["testing"].get(key)
    f = summary["feature"].get(key)
    if isinstance(t, (int, float)) and isinstance(f, (int, float)):
        return f - t
    return None


def compute_valgrind_delta(summary: dict[str, Any], section: str, field: str) -> int | None:
    testing = (((summary.get("testing") or {}).get("valgrind") or {}).get(section) or {}).get(field)
    feature = (((summary.get("feature") or {}).get("valgrind") or {}).get(section) or {}).get(field)
    if isinstance(testing, int) and isinstance(feature, int):
        return feature - testing
    return None


def main() -> int:
    args = parse_args()

    for app in (args.testing_app, args.feature_app):
        if not Path(app).exists():
            print(f"Missing app binary: {app}", file=sys.stderr)
            return 2

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    testing = run_capture(
        label="testing",
        app_path=args.testing_app,
        info_url=args.info_url,
        startup_wait_seconds=args.startup_wait,
        sample_count=args.samples,
        interval_seconds=args.interval,
        request_timeout_seconds=args.request_timeout,
        output_dir=out_dir,
        valgrind_enabled=not args.no_valgrind,
        valgrind_tool=args.valgrind_tool,
        valgrind_args=args.valgrind_arg,
        xtree_top_n=args.xtree_top,
    )
    feature = run_capture(
        label="feature",
        app_path=args.feature_app,
        info_url=args.info_url,
        startup_wait_seconds=args.startup_wait,
        sample_count=args.samples,
        interval_seconds=args.interval,
        request_timeout_seconds=args.request_timeout,
        output_dir=out_dir,
        valgrind_enabled=not args.no_valgrind,
        valgrind_tool=args.valgrind_tool,
        valgrind_args=args.valgrind_arg,
        xtree_top_n=args.xtree_top,
    )

    summary = {
        "testing": testing,
        "feature": feature,
        "delta_feature_minus_testing": {
            "heap_free_median": compute_delta({"testing": testing, "feature": feature}, "heap_free_median"),
            "minimumfreeHeapRuntime_min": compute_delta(
                {"testing": testing, "feature": feature}, "minimumfreeHeapRuntime_min"
            ),
            "minimumfreeHeap10min_min": compute_delta(
                {"testing": testing, "feature": feature}, "minimumfreeHeap10min_min"
            ),
            "valgrind_total_allocated_bytes": compute_valgrind_delta(
                {"testing": testing, "feature": feature}, "total_heap_usage", "bytes"
            ),
            "valgrind_total_allocs": compute_valgrind_delta(
                {"testing": testing, "feature": feature}, "total_heap_usage", "allocs"
            ),
            "valgrind_in_use_at_exit_bytes": compute_valgrind_delta(
                {"testing": testing, "feature": feature}, "in_use_at_exit", "bytes"
            ),
            "valgrind_possibly_lost_bytes": compute_valgrind_delta(
                {"testing": testing, "feature": feature}, "possibly_lost", "bytes"
            ),
            "valgrind_still_reachable_bytes": compute_valgrind_delta(
                {"testing": testing, "feature": feature}, "still_reachable", "bytes"
            ),
        },
    }

    summary_path = out_dir / "combined_report.json"
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    print(json.dumps(summary, indent=2))
    print(f"\nSummary written to: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
