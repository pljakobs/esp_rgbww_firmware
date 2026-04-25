#!/usr/bin/env python3
"""Collect /info?v=2 snapshots from two controllers for A/B memory analysis.

Features:
- Optional reboot of both controllers via POST /system {"cmd":"restart"}
- Polls /info?v=2 for a configured duration
- Writes compact CSV with key fields for analysis
- Writes JSONL with full raw payload per sample for forensics

Example:
    python3 test/collect_info_v2_log.py --reboot-before-start

With auth (if API security is enabled):
    python3 test/collect_info_v2_log.py --auth-user admin --auth-pass YOUR_PASSWORD
"""

from __future__ import annotations

import argparse
import base64
import csv
import json
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple
from urllib import error, request


DEFAULT_HOSTS = ("led-bu.fritz.box", "led-wo.fritz.box")


@dataclass
class SampleResult:
    host: str
    ok: bool
    http_status: int
    error: str
    payload: Dict[str, Any]


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def make_auth_header(user: Optional[str], password: Optional[str]) -> Optional[str]:
    if not password:
        return None
    user = user or "admin"
    token = f"{user}:{password}".encode("utf-8")
    return "Basic " + base64.b64encode(token).decode("ascii")


def http_json_request(
    url: str,
    method: str = "GET",
    body: Optional[Dict[str, Any]] = None,
    timeout: float = 5.0,
    auth_header: Optional[str] = None,
) -> Tuple[int, Dict[str, Any]]:
    data = None
    headers = {"Accept": "application/json"}
    if auth_header:
        headers["Authorization"] = auth_header

    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = request.Request(url, data=data, method=method, headers=headers)
    with request.urlopen(req, timeout=timeout) as resp:
        status = resp.getcode()
        raw = resp.read().decode("utf-8", errors="replace")

    try:
        payload = json.loads(raw)
        if isinstance(payload, dict):
            return status, payload
        return status, {"_non_object_json": payload}
    except json.JSONDecodeError:
        return status, {"_raw": raw}


def fetch_info(host: str, timeout: float, auth_header: Optional[str]) -> SampleResult:
    url = f"http://{host}/info?v=2"
    try:
        status, payload = http_json_request(url, timeout=timeout, auth_header=auth_header)
        return SampleResult(host=host, ok=(status == 200), http_status=status, error="", payload=payload)
    except error.HTTPError as exc:
        msg = exc.read().decode("utf-8", errors="replace") if exc.fp else str(exc)
        return SampleResult(host=host, ok=False, http_status=exc.code, error=msg.strip(), payload={})
    except Exception as exc:  # noqa: BLE001
        return SampleResult(host=host, ok=False, http_status=0, error=str(exc), payload={})


def fetch_config(host: str, timeout: float, auth_header: Optional[str]) -> SampleResult:
    url = f"http://{host}/config"
    try:
        status, payload = http_json_request(url, timeout=timeout, auth_header=auth_header)
        return SampleResult(host=host, ok=(status == 200), http_status=status, error="", payload=payload)
    except error.HTTPError as exc:
        msg = exc.read().decode("utf-8", errors="replace") if exc.fp else str(exc)
        return SampleResult(host=host, ok=False, http_status=exc.code, error=msg.strip(), payload={})
    except Exception as exc:  # noqa: BLE001
        return SampleResult(host=host, ok=False, http_status=0, error=str(exc), payload={})


def reboot_controller(host: str, timeout: float, auth_header: Optional[str]) -> SampleResult:
    url = f"http://{host}/system"
    body = {"cmd": "restart"}
    try:
        status, payload = http_json_request(
            url,
            method="POST",
            body=body,
            timeout=timeout,
            auth_header=auth_header,
        )
        ok = status == 200 and payload.get("success") is True
        err = "" if ok else json.dumps(payload, separators=(",", ":"))
        return SampleResult(host=host, ok=ok, http_status=status, error=err, payload=payload)
    except error.HTTPError as exc:
        msg = exc.read().decode("utf-8", errors="replace") if exc.fp else str(exc)
        return SampleResult(host=host, ok=False, http_status=exc.code, error=msg.strip(), payload={})
    except Exception as exc:  # noqa: BLE001
        return SampleResult(host=host, ok=False, http_status=0, error=str(exc), payload={})


def wait_until_info_available(
    hosts: Iterable[str],
    timeout_seconds: int,
    interval_seconds: float,
    request_timeout: float,
    auth_header: Optional[str],
) -> Dict[str, bool]:
    deadline = time.time() + timeout_seconds
    online = {host: False for host in hosts}

    while time.time() < deadline and not all(online.values()):
        for host in hosts:
            if online[host]:
                continue
            r = fetch_info(host, timeout=request_timeout, auth_header=auth_header)
            if r.ok:
                online[host] = True
        if not all(online.values()):
            time.sleep(interval_seconds)

    return online


def flatten_for_csv(result: SampleResult, timestamp_iso: str, epoch_s: int) -> Dict[str, Any]:
    p = result.payload if isinstance(result.payload, dict) else {}

    device = p.get("device", {}) if isinstance(p.get("device", {}), dict) else {}
    app = p.get("app", {}) if isinstance(p.get("app", {}), dict) else {}
    runtime = p.get("runtime", {}) if isinstance(p.get("runtime", {}), dict) else {}
    connection = p.get("connection", {}) if isinstance(p.get("connection", {}), dict) else {}
    network = p.get("network", {}) if isinstance(p.get("network", {}), dict) else {}
    debug = p.get("debug", {}) if isinstance(p.get("debug", {}), dict) else {}

    return {
        "timestamp_iso": timestamp_iso,
        "epoch_s": epoch_s,
        "host": result.host,
        "ok": int(result.ok),
        "http_status": result.http_status,
        "error": result.error,
        "deviceid": device.get("deviceid", ""),
        "soc": device.get("soc", ""),
        "current_rom": device.get("current_rom", ""),
        "git_version": app.get("git_version", ""),
        "build_type": app.get("build_type", ""),
        "git_date": app.get("git_date", ""),
        "uptime": runtime.get("uptime", ""),
        "heap_free": runtime.get("heap_free", ""),
        "minimumfreeHeapRuntime": runtime.get("minimumfreeHeapRuntime", ""),
        "minimumfreeHeap10min": runtime.get("minimumfreeHeap10min", ""),
        "heapLowErrUptime": runtime.get("heapLowErrUptime", ""),
        "heapLowErr10min": runtime.get("heapLowErr10min", ""),
        "connected": connection.get("connected", ""),
        "ip": connection.get("ip", ""),
        "rssi": connection.get("rssi", ""),
        "tcp_connections": network.get("tcp_connections", ""),
        "http_active_connections": debug.get("http_active_connections", ""),
        "websocket_connections": debug.get("websocket_connections", ""),
        "eventserver_clients": debug.get("eventserver_clients", ""),
        "syslog_pre_net_state": debug.get("syslog_pre_net_state", ""),
        "syslog_pre_net_buffer_allocated": debug.get("syslog_pre_net_buffer_allocated", ""),
        "syslog_pre_net_encoder_allocated": debug.get("syslog_pre_net_encoder_allocated", ""),
        "syslog_pre_net_buffer_capacity": debug.get("syslog_pre_net_buffer_capacity", ""),
        "syslog_pre_net_buffer_used": debug.get("syslog_pre_net_buffer_used", ""),
        "syslog_pre_net_buffer_frames": debug.get("syslog_pre_net_buffer_frames", ""),
        "syslog_pre_net_buffer_evicted": debug.get("syslog_pre_net_buffer_evicted", ""),
        "tcp_pcb_size": debug.get("tcp_pcb_size", ""),
        "tcp_active_estimated_bytes": debug.get("tcp_active_estimated_bytes", ""),
    }


def build_output_paths(out_dir: Path) -> Tuple[Path, Path]:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return out_dir / f"info_v2_trail_{stamp}.csv", out_dir / f"info_v2_trail_{stamp}.jsonl"


def sanitize_for_filename(host: str) -> str:
    return "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in host)


def collect_key_paths(obj: Any, prefix: str = "") -> Set[str]:
    paths: Set[str] = set()
    if isinstance(obj, dict):
        for key, value in obj.items():
            child = f"{prefix}.{key}" if prefix else key
            paths.add(child)
            paths.update(collect_key_paths(value, child))
    elif isinstance(obj, list):
        arr_path = f"{prefix}[]" if prefix else "[]"
        paths.add(arr_path)
        for value in obj:
            paths.update(collect_key_paths(value, arr_path))
    return paths


def write_config_structure_artifacts(
    hosts: List[str],
    out_dir: Path,
    label: str,
    request_timeout: float,
    auth_header: Optional[str],
) -> None:
    per_host_paths: Dict[str, Set[str]] = {}
    summary_lines: List[str] = [f"label={label}", f"timestamp_utc={utc_now_iso()}"]

    for host in hosts:
        result = fetch_config(host, timeout=request_timeout, auth_header=auth_header)
        safe_host = sanitize_for_filename(host)
        raw_path = out_dir / f"config_{label}_{safe_host}.json"
        paths_path = out_dir / f"config_paths_{label}_{safe_host}.txt"

        if result.ok and isinstance(result.payload, dict):
            raw_path.write_text(json.dumps(result.payload, indent=2, sort_keys=True), encoding="utf-8")
            paths = collect_key_paths(result.payload)
            per_host_paths[host] = paths
            paths_path.write_text("\n".join(sorted(paths)) + "\n", encoding="utf-8")
            summary_lines.append(f"{host}: ok status={result.http_status} keys={len(paths)}")
        else:
            raw_path.write_text(
                json.dumps(
                    {
                        "host": host,
                        "ok": result.ok,
                        "http_status": result.http_status,
                        "error": result.error,
                        "payload": result.payload,
                    },
                    indent=2,
                    sort_keys=True,
                ),
                encoding="utf-8",
            )
            paths_path.write_text("", encoding="utf-8")
            summary_lines.append(
                f"{host}: error status={result.http_status} err={result.error}"
            )

    compare_path = out_dir / f"config_paths_compare_{label}.txt"
    summary_path = out_dir / f"config_capture_{label}_summary.txt"

    if len(hosts) == 2:
        host_a, host_b = hosts[0], hosts[1]
        paths_a = per_host_paths.get(host_a, set())
        paths_b = per_host_paths.get(host_b, set())
        common = sorted(paths_a & paths_b)
        only_a = sorted(paths_a - paths_b)
        only_b = sorted(paths_b - paths_a)

        compare_lines: List[str] = []
        compare_lines.append(f"Compare label={label}")
        compare_lines.append(f"Host A: {host_a}")
        compare_lines.append(f"Host B: {host_b}")
        compare_lines.append(f"common={len(common)}")
        compare_lines.append(f"only_{host_a}={len(only_a)}")
        compare_lines.append(f"only_{host_b}={len(only_b)}")
        compare_lines.append("")
        compare_lines.append("[only_host_a]")
        compare_lines.extend(only_a)
        compare_lines.append("")
        compare_lines.append("[only_host_b]")
        compare_lines.extend(only_b)
        compare_lines.append("")
        compare_lines.append("[common]")
        compare_lines.extend(common)
        compare_path.write_text("\n".join(compare_lines) + "\n", encoding="utf-8")
    else:
        compare_path.write_text(
            "Host comparison file is only generated for exactly two hosts.\n",
            encoding="utf-8",
        )

    summary_path.write_text("\n".join(summary_lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Collect /info?v=2 from controllers for one hour")
    p.add_argument("--hosts", nargs="+", default=list(DEFAULT_HOSTS), help="Hosts to poll")
    p.add_argument("--duration", type=int, default=3600, help="Collection duration in seconds")
    p.add_argument("--interval", type=float, default=5.0, help="Polling interval in seconds")
    p.add_argument("--request-timeout", type=float, default=5.0, help="HTTP request timeout in seconds")
    p.add_argument("--out-dir", default=".", help="Output directory for CSV/JSONL")
    p.add_argument("--auth-user", default="admin", help="HTTP Basic auth user")
    p.add_argument("--auth-pass", default=None, help="HTTP Basic auth password")
    p.add_argument(
        "--reboot-before-start",
        action="store_true",
        help="Reboot all hosts before starting collection",
    )
    p.add_argument(
        "--reboot-wait-timeout",
        type=int,
        default=180,
        help="Max seconds to wait for hosts to come back after reboot",
    )
    p.add_argument(
        "--reboot-grace-seconds",
        type=float,
        default=8.0,
        help="Sleep after sending reboot before probing availability",
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    hosts = list(dict.fromkeys(args.hosts))
    if not hosts:
        print("No hosts provided", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path, jsonl_path = build_output_paths(out_dir)

    auth_header = make_auth_header(args.auth_user, args.auth_pass)

    print(f"Hosts: {', '.join(hosts)}")
    print(f"Duration: {args.duration}s, interval: {args.interval}s")
    print(f"CSV: {csv_path}")
    print(f"JSONL: {jsonl_path}")

    if args.reboot_before_start:
        print("Rebooting controllers before collection...")
        for host in hosts:
            r = reboot_controller(host, timeout=args.request_timeout, auth_header=auth_header)
            if r.ok:
                print(f"  [ok] reboot command accepted by {host}")
            else:
                print(
                    f"  [warn] reboot command failed for {host}: "
                    f"status={r.http_status} err={r.error}"
                )

        time.sleep(max(0.0, args.reboot_grace_seconds))
        online = wait_until_info_available(
            hosts,
            timeout_seconds=args.reboot_wait_timeout,
            interval_seconds=2.0,
            request_timeout=args.request_timeout,
            auth_header=auth_header,
        )
        for host, is_online in online.items():
            state = "online" if is_online else "not online"
            print(f"  [{state}] {host}")

    print("Capturing /config structure snapshot (start)...")
    write_config_structure_artifacts(
        hosts=hosts,
        out_dir=out_dir,
        label="start",
        request_timeout=args.request_timeout,
        auth_header=auth_header,
    )

    fieldnames = list(
        flatten_for_csv(SampleResult(host="", ok=False, http_status=0, error="", payload={}), "", 0).keys()
    )

    deadline = time.time() + args.duration
    samples = 0

    with csv_path.open("w", newline="", encoding="utf-8") as csv_file, jsonl_path.open("w", encoding="utf-8") as jsonl_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()

        while time.time() < deadline:
            loop_start = time.time()
            timestamp_iso = utc_now_iso()
            epoch_s = int(loop_start)

            for host in hosts:
                result = fetch_info(host, timeout=args.request_timeout, auth_header=auth_header)
                row = flatten_for_csv(result, timestamp_iso=timestamp_iso, epoch_s=epoch_s)
                writer.writerow(row)

                jsonl_record = {
                    "timestamp_iso": timestamp_iso,
                    "epoch_s": epoch_s,
                    "host": host,
                    "ok": result.ok,
                    "http_status": result.http_status,
                    "error": result.error,
                    "payload": result.payload,
                }
                jsonl_file.write(json.dumps(jsonl_record, separators=(",", ":")) + "\n")

                status_txt = "ok" if result.ok else "err"
                heap = ""
                uptime = ""
                if result.payload:
                    runtime = result.payload.get("runtime", {})
                    if isinstance(runtime, dict):
                        heap = runtime.get("heap_free", "")
                        uptime = runtime.get("uptime", "")
                print(
                    f"[{timestamp_iso}] {host} {status_txt} "
                    f"status={result.http_status} heap={heap} uptime={uptime}"
                )
                samples += 1

            csv_file.flush()
            jsonl_file.flush()

            elapsed = time.time() - loop_start
            sleep_for = args.interval - elapsed
            if sleep_for > 0:
                time.sleep(sleep_for)

    print(f"Done. Wrote {samples} samples.")
    print(f"CSV: {csv_path}")
    print(f"JSONL: {jsonl_path}")

    print("Capturing /config structure snapshot (end)...")
    write_config_structure_artifacts(
        hosts=hosts,
        out_dir=out_dir,
        label="end",
        request_timeout=args.request_timeout,
        auth_header=auth_header,
    )
    print(f"Config artifacts written in: {out_dir}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
