#!/usr/bin/env python3
"""Poll /info from two hosts at a fixed interval and append results to CSV."""

from __future__ import annotations

import argparse
import csv
import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen


DEFAULT_TIMEOUT_SECONDS = 3.0
DEFAULT_REBOOT_WAIT_SECONDS = 8.0


def _lower_key_map(payload: dict[str, Any]) -> dict[str, Any]:
    return {str(k).lower(): v for k, v in payload.items()}


def _pick(payload: dict[str, Any], *keys: str) -> Any:
    if not payload:
        return ""
    lowered = _lower_key_map(payload)
    for key in keys:
        if key in payload:
            return payload[key]
        value = lowered.get(key.lower())
        if value is not None:
            return value
    return ""


def normalize_host(value: str) -> str:
    value = value.strip()
    if not value:
        raise ValueError("Host must not be empty")

    parsed = urlparse(value)
    if parsed.scheme:
        return value.rstrip("/")
    return f"http://{value}"


def build_url(base: str, path: str) -> str:
    return f"{base.rstrip('/')}/{path.lstrip('/')}"


def get_json(url: str, timeout_seconds: float) -> tuple[int | None, dict[str, Any] | None, str | None]:
    req = Request(url, method="GET")
    try:
        with urlopen(req, timeout=timeout_seconds) as resp:
            status = int(resp.status)
            raw = resp.read().decode("utf-8", errors="replace")
            data = json.loads(raw)
            if isinstance(data, dict):
                return status, data, None
            return status, None, f"unexpected json root type: {type(data).__name__}"
    except HTTPError as err:
        return err.code, None, f"http error: {err.reason}"
    except URLError as err:
        return None, None, f"url error: {err.reason}"
    except TimeoutError:
        return None, None, "timeout"
    except json.JSONDecodeError as err:
        return None, None, f"json decode error: {err}"
    except Exception as err:  # pragma: no cover - defensive fallback
        return None, None, f"unexpected error: {err}"


def post_restart(system_url: str, timeout_seconds: float) -> tuple[int | None, str | None]:
    payload = json.dumps({"cmd": "restart"}).encode("utf-8")
    req = Request(
        system_url,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urlopen(req, timeout=timeout_seconds) as resp:
            return int(resp.status), None
    except HTTPError as err:
        return err.code, f"http error: {err.reason}"
    except URLError as err:
        return None, f"url error: {err.reason}"
    except TimeoutError:
        return None, "timeout"
    except Exception as err:  # pragma: no cover - defensive fallback
        return None, f"unexpected error: {err}"


def row_for_host(
    host_label: str,
    base_url: str,
    info_path: str,
    timeout_seconds: float,
) -> dict[str, Any]:
    url = build_url(base_url, info_path)
    status, data, error = get_json(url, timeout_seconds)

    now = datetime.now(timezone.utc)
    row: dict[str, Any] = {
        "timestamp_utc": now.isoformat(),
        "epoch_seconds": int(now.timestamp()),
        "host_label": host_label,
        "host": base_url,
        "url": url,
        "http_status": status if status is not None else "",
        "error": error or "",
    }

    if not data:
        row.update(
            {
                "deviceid": "",
                "soc": "",
                "current_rom": "",
                "build_type": "",
                "git_version": "",
                "uptime": "",
                "heap_free": "",
                "minimumfreeHeapRuntime": "",
                "minimumfreeHeap10min": "",
                "ws_clients": "",
                "http_active_connections": "",
                "events_num_clients": "",
            }
        )
        return row

    device = data.get("device", {}) if isinstance(data.get("device", {}), dict) else {}
    app = data.get("app", {}) if isinstance(data.get("app", {}), dict) else {}
    runtime = data.get("runtime", {}) if isinstance(data.get("runtime", {}), dict) else {}
    debug = data.get("debug", {}) if isinstance(data.get("debug", {}), dict) else {}

    # Some firmware versions return flat /info JSON (legacy) instead of v2 nested objects.
    # Read nested first, then fall back to flat aliases.
    deviceid = _pick(device, "deviceid", "device_id") or _pick(data, "deviceid", "device_id")
    soc = _pick(device, "soc") or _pick(data, "soc")
    current_rom = _pick(device, "current_rom", "currentRom", "currentrom") or _pick(
        data, "current_rom", "currentRom", "currentrom"
    )

    build_type = _pick(app, "build_type", "buildType") or _pick(data, "build_type", "buildType")
    git_version = _pick(app, "git_version", "gitVersion") or _pick(data, "git_version", "gitVersion")

    uptime = _pick(runtime, "uptime") or _pick(data, "uptime")
    heap_free = _pick(runtime, "heap_free", "heapFree") or _pick(data, "heap_free", "heapFree")
    minimumfree_heap_runtime = _pick(runtime, "minimumfreeHeapRuntime") or _pick(
        data, "minimumfreeHeapRuntime"
    )
    minimumfree_heap_10min = _pick(runtime, "minimumfreeHeap10min") or _pick(
        data, "minimumfreeHeap10min"
    )
    events_num_clients = _pick(runtime, "event_num_clients", "eventNumClients") or _pick(
        data, "event_num_clients", "eventNumClients"
    )

    ws_clients = _pick(debug, "wsClients", "websocket_connections") or _pick(
        data, "wsClients", "websocket_connections"
    )
    http_active_connections = _pick(debug, "http_active_connections", "httpActiveConnections") or _pick(
        data, "http_active_connections", "httpActiveConnections"
    )

    row.update(
        {
            "deviceid": deviceid,
            "soc": soc,
            "current_rom": current_rom,
            "build_type": build_type,
            "git_version": git_version,
            "uptime": uptime,
            "heap_free": heap_free,
            "minimumfreeHeapRuntime": minimumfree_heap_runtime,
            "minimumfreeHeap10min": minimumfree_heap_10min,
            "ws_clients": ws_clients,
            "http_active_connections": http_active_connections,
            "events_num_clients": events_num_clients,
        }
    )
    return row


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Poll /info from two hosts and write one CSV row per host per interval."
        )
    )
    parser.add_argument("host1", help="First host, e.g. 192.168.29.31 or http://192.168.29.31")
    parser.add_argument("host2", help="Second host, e.g. 192.168.29.32 or http://192.168.29.32")
    parser.add_argument("interval_seconds", type=float, help="Polling interval in seconds")
    parser.add_argument(
        "--output",
        default="out/host-ci/info_capture.csv",
        help="Output CSV path (default: out/host-ci/info_capture.csv)",
    )
    parser.add_argument(
        "--info-path",
        default="/info?v=2",
        help="Info endpoint path (default: /info?v=2)",
    )
    parser.add_argument(
        "--reboot",
        action="store_true",
        help="Send POST /system {\"cmd\":\"restart\"} to both hosts before polling",
    )
    parser.add_argument(
        "--reboot-wait",
        type=float,
        default=DEFAULT_REBOOT_WAIT_SECONDS,
        help=f"Seconds to wait after reboot command (default: {DEFAULT_REBOOT_WAIT_SECONDS})",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help=f"HTTP timeout seconds (default: {DEFAULT_TIMEOUT_SECONDS})",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.interval_seconds <= 0:
        print("interval_seconds must be > 0", file=sys.stderr)
        return 2

    host1 = normalize_host(args.host1)
    host2 = normalize_host(args.host2)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    fieldnames = [
        "timestamp_utc",
        "epoch_seconds",
        "host_label",
        "host",
        "url",
        "http_status",
        "error",
        "deviceid",
        "soc",
        "current_rom",
        "build_type",
        "git_version",
        "uptime",
        "heap_free",
        "minimumfreeHeapRuntime",
        "minimumfreeHeap10min",
        "ws_clients",
        "http_active_connections",
        "events_num_clients",
    ]

    if args.reboot:
        for label, host in (("host1", host1), ("host2", host2)):
            status, err = post_restart(build_url(host, "/system"), args.timeout)
            if err:
                print(f"[{label}] reboot request failed: {err}", file=sys.stderr)
            else:
                print(f"[{label}] reboot request sent: HTTP {status}")
        print(f"Waiting {args.reboot_wait:.1f}s after reboot request...")
        time.sleep(args.reboot_wait)

    file_exists = output_path.exists()
    with output_path.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        if not file_exists:
            writer.writeheader()

        print(
            "Capturing /info to CSV. Press Ctrl+C to stop. "
            f"Output: {output_path}"
        )

        try:
            while True:
                cycle_start = time.time()
                writer.writerow(
                    row_for_host("host1", host1, args.info_path, args.timeout)
                )
                writer.writerow(
                    row_for_host("host2", host2, args.info_path, args.timeout)
                )
                f.flush()

                elapsed = time.time() - cycle_start
                sleep_for = args.interval_seconds - elapsed
                if sleep_for > 0:
                    time.sleep(sleep_for)
        except KeyboardInterrupt:
            print("\nStopped by user.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
