#!/usr/bin/env python3
"""Report controller statistics from retained lightinator-log-service logs.

Example:
    python3 tests/log_statistics.py --host lightinator.local --log 192.168.29.5 --callback-timing -b 0

Add a statistic by defining cli_option, search_term and implementing consume()
and report(sort, timeline), then registering its class in STATISTICS. Only the log service
is queried; the controller is not modified or rebooted.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import pathlib
import re
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Iterator
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any, Protocol


def log_service_url(address: str) -> str:
    if "://" not in address:
        try:
            ip = ipaddress.ip_address(address)
        except ValueError:
            pass
        else:
            if ip.version == 6:
                address = f"[{address}]"
        address = f"http://{address}"
    parts = urllib.parse.urlsplit(address)
    if (parts.scheme not in {"http", "https"} or not parts.hostname
            or parts.username is not None or parts.password is not None
            or parts.path not in {"", "/"} or parts.query or parts.fragment):
        raise ValueError("--log must be a hostname or address, optionally with a port or HTTP(S) scheme")
    port = parts.port if parts.port is not None else 4821
    if not 1 <= port <= 65535:
        raise ValueError("--log port must be between 1 and 65535")
    host = f"[{parts.hostname}]" if ":" in parts.hostname else parts.hostname
    return f"{parts.scheme}://{host}:{port}"


def controller_ip(host: str) -> str:
    try:
        return str(ipaddress.ip_address(host))
    except ValueError:
        addresses = {
            result[4][0]
            for result in socket.getaddrinfo(host, None, type=socket.SOCK_DGRAM)
        }
        if len(addresses) != 1:
            raise ValueError(
                f"--host resolves to multiple addresses: {', '.join(sorted(addresses))}; "
                "specify the controller's syslog source IP"
            )
        return addresses.pop()


@dataclass(frozen=True)
class LogRecord:
    id: int
    message: str
    boot: str
    boot_number: int | None = None
    received_at: float | None = None

    @classmethod
    def from_json(cls, value: Any) -> LogRecord:
        if (not isinstance(value, dict)
                or type(value.get("id")) is not int or value["id"] <= 0
                or not isinstance(value.get("message"), str)):
            raise ValueError(f"Invalid log record: {value!r}")
        nonce = value.get("bootNonce")
        boot = value.get("boot")
        if boot is not None and type(boot) is not int:
            raise ValueError(f"Invalid boot number in log {value['id']}: {boot!r}")
        if nonce is not None:
            identity = f"nonce:{nonce}"
        elif boot is not None:
            identity = f"boot:{boot}"
        else:
            identity = "unknown"
        received_at = None
        if value.get("receivedAt") is not None:
            try:
                stamp = datetime.fromisoformat(value["receivedAt"].replace("Z", "+00:00"))
                if stamp.tzinfo is None:
                    raise ValueError("receive timestamp lacks timezone")
                received_at = stamp.timestamp()
            except (TypeError, AttributeError, ValueError) as err:
                raise ValueError(f"Invalid receivedAt in log {value['id']}") from err
        return cls(value["id"], value["message"], identity, boot, received_at)


@dataclass(frozen=True)
class BootSelection:
    number: int
    first_id: int
    last_id: int


class LogService:
    def __init__(self, base_url: str):
        self.base_url = base_url

    def get(self, path: str, **params: str | int) -> dict[str, Any]:
        url = f"{self.base_url}{path}?{urllib.parse.urlencode(params)}"
        with urllib.request.urlopen(url, timeout=10) as response:
            result = json.load(response)
        if not isinstance(result, dict):
            raise ValueError(f"Expected a JSON object from {url}")
        return result

    def select_boot(self, ip: str, offset: int) -> BootSelection:
        if offset > 0:
            raise ValueError("Boot offset must be 0 (current) or negative (previous boots)")
        page = self.get("/api/v1/boots", ip=ip)
        items = page.get("items")
        if not isinstance(items, list):
            raise ValueError("Log service boot response is missing an items array")
        boots: dict[int, BootSelection] = {}
        for item in items:
            if (not isinstance(item, dict)
                    or any(type(item.get(key)) is not int for key in ("boot", "firstId", "lastId"))
                    or item["boot"] < 0 or not 0 < item["firstId"] <= item["lastId"]
                    or item["boot"] in boots):
                raise ValueError(f"Invalid boot metadata: {item!r}")
            boots[item["boot"]] = BootSelection(item["boot"], item["firstId"], item["lastId"])
        ordered = sorted(boots, reverse=True)
        if -offset >= len(ordered):
            raise ValueError(f"Boot offset {offset} is unavailable; {len(ordered)} retained boot(s)")
        return boots[ordered[-offset]]

    def search(self, ip: str, terms: tuple[str, ...]) -> list[LogRecord] | None:
        matches: dict[int, LogRecord] = {}
        for term in terms:
            page = self.get("/api/v1/search", q=term, limit=200, context=1)
            snippets = page.get("snippets")
            if not isinstance(snippets, list):
                raise ValueError("Log service search response is missing a snippets array")
            if len(snippets) >= 200:
                print(
                    "Search reached the 200-match limit; reading paginated controller logs "
                    "to avoid truncated statistics.",
                    file=sys.stderr,
                )
                return None
            for snippet in snippets:
                if (not isinstance(snippet, dict) or not isinstance(snippet.get("ip"), str)
                        or type(snippet.get("matchId")) is not int
                        or not isinstance(snippet.get("rows"), list)):
                    raise ValueError(f"Invalid log search snippet: {snippet!r}")
                if snippet["ip"] != ip:
                    continue
                rows = [LogRecord.from_json(row) for row in snippet["rows"]]
                matching = [row for row in rows if row.id == snippet["matchId"]]
                if len(matching) != 1:
                    raise ValueError(f"Search snippet lacks its matching log: {snippet['matchId']}")
                record = matching[0]
                if term in record.message:
                    matches[record.id] = record
        return sorted(matches.values(), key=lambda record: record.id)

    def logs(
        self, ip: str, terms: tuple[str, ...], boot: BootSelection | None = None,
    ) -> Iterator[LogRecord]:
        matches = self.search(ip, terms)
        if matches is not None:
            yield from (
                record for record in matches
                if boot is None or (
                    record.boot_number == boot.number and boot.first_id <= record.id <= boot.last_id
                )
            )
            return
        if boot is None:
            # Fix the end of the query so ongoing logging cannot extend it forever.
            tail = self.get("/api/v1/logs", ip=ip, limit=1)
            latest = self.records(tail)
            if not latest:
                return
            end_id = latest[-1].id
            cursor = 1
        else:
            end_id = boot.last_id
            cursor = boot.first_id
        previous_id = 0
        while True:
            page = self.get("/api/v1/logs", ip=ip, limit=2000, **{"from": cursor})
            records = self.records(page)
            for record in records:
                if record.id <= previous_id or record.id < cursor:
                    raise ValueError("Log service returned non-increasing log IDs")
                if record.id > end_id:
                    return
                previous_id = record.id
                if ((boot is None or record.boot_number == boot.number)
                        and any(term in record.message for term in terms)):
                    yield record
                if record.id == end_id:
                    return
            next_after = page.get("nextAfter")
            if next_after is None:
                return
            if (type(next_after) is not int
                    or next_after <= max(cursor, previous_id) or not records):
                raise ValueError(f"Invalid log pagination cursor: {next_after!r}")
            cursor = next_after

    @staticmethod
    def records(page: dict[str, Any]) -> list[LogRecord]:
        items = page.get("items")
        if not isinstance(items, list):
            raise ValueError("Log service response is missing an items array")
        return [LogRecord.from_json(item) for item in items]


class Statistic(Protocol):
    cli_option: str
    search_term: str

    def consume(self, record: LogRecord) -> None: ...

    def report(self, sort: str = "average", timeline: str | None = None) -> str: ...

    def plot_data(self) -> dict[str, Any] | None: ...


@dataclass(frozen=True)
class Timing:
    calls: int
    max_us: int
    total_us: int


@dataclass
class TimingWindow:
    window_ms: int | None = None
    functions: dict[str, Timing] = field(default_factory=dict)
    dropped_scopes: int = 0
    format_version: int = 1
    received_at: float | None = None


class CallbackTiming:
    cli_option = "callback-timing"
    search_term = "CallbackTiming"
    marker = re.compile(r"\bCallbackTiming\b")
    included_in = {
        "light_show": "led_update",
        "light_state": "led_update",
        "current_state": "led_update",
        "runtime_collect": "runtime_info",
        "runtime_json": "runtime_info",
        "runtime_enqueue": "runtime_info",
        "ota_hash": "ota_verify",
        "ws_pump": "synchronous caller",
    }

    def __init__(self) -> None:
        self.windows: dict[tuple[str, int], TimingWindow] = {}
        self.explicit_paths: set[str] = set()

    def parent(self, name: str) -> str:
        if name in self.explicit_paths or "/" in name:
            return name.rpartition("/")[0] or "-"
        return self.included_in.get(name, "-")

    def consume(self, record: LogRecord) -> None:
        match = self.marker.search(record.message)
        if match is None:
            return
        fields: dict[str, str] = {}
        for token in record.message[match.end():].split():
            key, separator, value = token.partition("=")
            if not separator or not value or key in fields:
                raise ValueError(f"Malformed CallbackTiming log {record.id}: {record.message}")
            fields[key] = value
        try:
            sequence = int(fields["seq"])
            if sequence < 0:
                raise ValueError("negative sequence")
            version = int(fields.get("format", "1"))
            if version not in {1, 2}:
                raise ValueError("unsupported measurement format")
            dropped = int(fields.get("dropped_scopes", "0"))
            if dropped < 0:
                raise ValueError("negative dropped scope count")
            duration = None
            timing = None
            if "name" in fields:
                if not re.fullmatch(r"[a-z][a-z0-9_]*(/[a-z][a-z0-9_]*)*", fields["name"]):
                    raise ValueError("invalid measurement path")
                timing = Timing(int(fields["calls"]), int(fields["max_us"]), int(fields["total_us"]))
                average = float(fields["avg_us"])
                if (timing.calls <= 0 or timing.max_us < 0 or timing.total_us < 0
                        or not 0 <= average <= timing.max_us
                        or not timing.max_us <= timing.total_us <= timing.calls * timing.max_us):
                    raise ValueError("invalid timing counters")
            else:
                duration = int(fields["window_ms"])
                if duration <= 0:
                    raise ValueError("non-positive window")
        except (KeyError, ValueError) as err:
            raise ValueError(f"Malformed CallbackTiming log {record.id}: {record.message}") from err
        window = self.windows.setdefault((record.boot, sequence), TimingWindow())
        if record.received_at is not None:
            window.received_at = (record.received_at if window.received_at is None
                                  else min(window.received_at, record.received_at))
        if version == 2:
            window.format_version = 2
            self.explicit_paths.update(window.functions)
        if timing is not None:
            name = fields["name"]
            if window.format_version == 2 or "/" in name:
                self.explicit_paths.add(name)
            existing = window.functions.get(name)
            if existing is not None and existing != timing:
                raise ValueError(f"Conflicting CallbackTiming log {record.id} for {name}")
            window.functions[name] = timing
        else:
            if window.window_ms is not None and window.window_ms != duration:
                raise ValueError(f"Conflicting CallbackTiming window in log {record.id}")
            window.window_ms = duration
            window.dropped_scopes = dropped

    def report(self, sort: str = "average", timeline: str | None = None) -> str:
        if sort not in SORT_COLUMNS:
            raise ValueError(f"Invalid sort column: {sort}")
        if timeline is not None and timeline not in TIMELINE_METRICS:
            raise ValueError(f"Invalid timeline metric: {timeline}")
        lines = ["CallbackTiming"]
        if not self.windows:
            return "\n".join([*lines, "No matching logs found."])
        sequences: dict[str, list[int]] = {}
        totals: dict[str, Timing] = {}
        estimated = 0
        elapsed_ms = 0
        for (boot, sequence), window in self.windows.items():
            sequences.setdefault(boot, []).append(sequence)
            if window.window_ms is None:
                estimated += 1
                elapsed_ms += 5000
            else:
                elapsed_ms += window.window_ms
            for name, timing in window.functions.items():
                previous = totals.get(name, Timing(0, 0, 0))
                totals[name] = Timing(
                    previous.calls + timing.calls,
                    max(previous.max_us, timing.max_us),
                    previous.total_us + timing.total_us,
                )
        missing = sum(max(values) - min(values) + 1 - len(values) for values in sequences.values())
        lines.append(
            f"Observed blocks: {len(self.windows)}; boots: {len(sequences)}; "
            f"missing sequence blocks: {missing}"
        )
        lines.append(f"Observed window time: {elapsed_ms / 1000:.3f} s "
                     f"({estimated} missing headers estimated at 5 s)")
        long_windows = [(boot, sequence) for (boot, sequence), window in self.windows.items()
                        if window.window_ms is not None and window.window_ms > 60000]
        if long_windows:
            lines.append("WARNING: Reporting windows exceed 60 s; verify firmware timing before interpreting "
                         "rates or timeline: " + ", ".join(
                             f"{boot} seq={sequence}" for boot, sequence in long_windows
                         ))
        if "unknown" in sequences:
            lines.append("WARNING: Some logs lack boot identity; repeated sequences cannot be distinguished.")
        lines.append("Times are inclusive, in milliseconds; averages are total time / calls.")
        lines.append("Calls/s and ms/s divide by all observed window time, including inactive blocks; "
                     "missing sequences are excluded. Rates may be understated by lost records.")
        lines.append("Only received records contribute; UDP loss may leave blocks incomplete.")
        dropped = sum(window.dropped_scopes for window in self.windows.values())
        if dropped:
            lines.append(f"WARNING: Firmware dropped {dropped} scopes due to timing path/depth capacity limits.")
        lines.extend([
            "Timing hierarchy (do not sum parents and children or infer exclusive time):",
            "  Format 2 paths encode actual synchronous nesting; siblings are separate scoped operations.",
            "  Format 1 names use legacy parent hints; roots are not necessarily independent CPU work.",
            "  Counts belong to completion windows: parent/child totals may cross window boundaries.",
            "  led_update -> light_show + publication trigger checks (no JSON/sends)",
            "  state_publish -> one deferred light_state, current_state or MQTT message",
            "  runtime_info -> runtime_collect, runtime_json, runtime_enqueue + logging/overhead",
            "  ota_verify -> ota_hash",
            "  timing_report -> diagnostic formatting/syslog, including frozen trace output",
            "  ws_pump -> stream pumping; initial synchronous work also belongs to its caller",
            "  light_show, light_state, current_state, runtime subsections, ota_hash:",
            "    legacy leaves; state publication now measures root/populate/render/normalize/send",
            "  light_reload, telemetry, buttons, mqtt_receive -> whole operation, including synchronous callees",
            "  DispatcherTiming (not graphed) includes synchronous batch work + dispatcher overhead.",
            "  Deferred callbacks belong to later batches; TCP/IP MQTT reception need not belong to a batch.",
            "  All durations include blocking and preemption; nested load curves are not additive.",
        ])
        lines.append("Included in: known CallbackTiming parent; '-' = no fixed parent, not independent work.")
        labels = {
            name: ("  " * name.count("/") + name.rsplit("/", 1)[-1]) if sort == "tree" else name
            for name in totals
        }
        width = max([8, *(len(label) for label in labels.values())])
        lines.append(
            f"{'Function':<{width}} {'Calls':>12} {'Average ms':>14} "
            f"{'Max ms':>12} {'Total ms':>16} {'Calls/s':>12} {'ms/s':>12}  Included in"
        )

        def sort_key(item: tuple[str, Timing]) -> tuple[float, str, str]:
            name, timing = item
            if sort == "included-in":
                return 0, self.parent(name), name
            if sort == "tree":
                return 0, name, name
            value = {
                "function": 0,
                "calls": timing.calls,
                "average": timing.total_us / timing.calls,
                "max": timing.max_us,
                "total": timing.total_us,
                "calls/s": timing.calls * 1000 / elapsed_ms,
                "ms/s": timing.total_us / elapsed_ms,
            }[sort]
            return -value, "", name

        ordered = sorted(totals.items(), key=sort_key)
        for name, timing in ordered:
            lines.append(
                f"{labels[name]:<{width}} {timing.calls:>12} {timing.total_us / timing.calls / 1000:>14.3f} "
                f"{timing.max_us / 1000:>12.3f} {timing.total_us / 1000:>16.3f}  "
                f"{timing.calls * 1000 / elapsed_ms:>12.3f} {timing.total_us / elapsed_ms:>12.3f}  "
                f"{self.parent(name)}"
            )
        return "\n".join(lines)

    def plot_data(self) -> dict[str, Any]:
        boots: dict[str, dict[int, TimingWindow]] = {}
        for (boot, sequence), window in self.windows.items():
            boots.setdefault(boot, {})[sequence] = window
        result = []
        for boot, windows in boots.items():
            blocks = []
            elapsed = 0.0
            previous = None
            for sequence, window in sorted(windows.items()):
                gap = sequence - previous - 1 if previous is not None else 0
                elapsed += gap * 5
                duration_ms = window.window_ms or 5000
                functions = {
                    name: {
                        "load": timing.total_us / 1000 / duration_ms * 100,
                        "total": timing.total_us / 1000,
                        "calls": timing.calls,
                        "calls/s": timing.calls * 1000 / duration_ms,
                        "ms/s": timing.total_us / duration_ms,
                    }
                    for name, timing in window.functions.items()
                }
                blocks.append({
                    "seq": sequence, "start": elapsed, "end": elapsed + duration_ms / 1000,
                    "estimated": window.window_ms is None, "gap": gap, "functions": functions,
                    "received_at": window.received_at,
                    "window_ms": window.window_ms,
                })
                elapsed += duration_ms / 1000
                previous = sequence
            result.append({"name": boot, "duration": elapsed, "blocks": blocks})
        names = {name for window in self.windows.values() for name in window.functions}
        return {
            "name": self.search_term,
            "parents": {name: self.parent(name) for name in names},
            "boots": result,
        }


class DispatcherTiming:
    cli_option = "dispatcher-timing"
    search_term = "DispatcherTiming"
    marker = re.compile(r"\bDispatcherTiming\b")
    counters = (
        "batches", "events", "max_us", "total_us", "max_events",
        "queue_peak_sampled", "event_full", "mbox_attempts", "mbox_full",
    )

    def __init__(self) -> None:
        self.windows: dict[tuple[str, int], dict[str, int]] = {}

    def consume(self, record: LogRecord) -> None:
        match = self.marker.search(record.message)
        if match is None:
            return
        fields: dict[str, str] = {}
        try:
            for token in record.message[match.end():].split():
                key, separator, value = token.partition("=")
                if not separator or not value or key in fields:
                    raise ValueError("invalid or duplicate field")
                fields[key] = value
            sequence = int(fields["seq"])
            counters = {key: int(fields[key]) for key in self.counters}
            average = float(fields["avg_us"])
            batches = counters["batches"]
            maximum = counters["max_us"]
            total = counters["total_us"]
            if (sequence < 0 or any(value < 0 for value in counters.values())
                    or not 0 <= average <= maximum
                    or not maximum <= total <= batches * maximum
                    or counters["mbox_full"] > counters["mbox_attempts"]
                    or counters["max_events"] > counters["events"]):
                raise ValueError("invalid dispatcher counters")
        except (KeyError, ValueError) as err:
            raise ValueError(f"Malformed DispatcherTiming log {record.id}: {record.message}") from err
        key = (record.boot, sequence)
        existing = self.windows.get(key)
        if existing is not None and existing != counters:
            raise ValueError(f"Conflicting DispatcherTiming log {record.id}")
        self.windows[key] = counters

    def report(self, sort: str = "average", timeline: str | None = None) -> str:
        lines = ["DispatcherTiming (event queue and TCP/IP mailbox)"]
        if not self.windows:
            return "\n".join([*lines, "No matching logs found."])
        sequences: dict[str, list[int]] = {}
        for boot, sequence in self.windows:
            sequences.setdefault(boot, []).append(sequence)
        missing = sum(max(values) - min(values) + 1 - len(values) for values in sequences.values())
        sums = {key: sum(window[key] for window in self.windows.values()) for key in self.counters}
        peaks = {key: max(window[key] for window in self.windows.values()) for key in self.counters}
        average = sums["total_us"] / sums["batches"] / 1000 if sums["batches"] else 0
        lines.extend([
            f"Observed blocks: {len(self.windows)}; boots: {len(sequences)}; missing sequence blocks: {missing}",
            "Only received reports contribute; counters reset per report, not cumulative since boot.",
            f"Dispatcher batches: {sums['batches']}; processed events: {sums['events']}",
            f"Batch duration ms: average {average:.3f}; max {peaks['max_us'] / 1000:.3f}; "
            f"total {sums['total_us'] / 1000:.3f}",
            f"Maximum events per batch: {peaks['max_events']}",
            f"Event queue sampled peak depth: {peaks['queue_peak_sampled']} (lower bound, not a continuous peak)",
            f"Event queue rejected posts (event_full): {sums['event_full']}",
            f"Mailbox post attempts (mbox_attempts): {sums['mbox_attempts']}",
            f"Mailbox rejected posts (mbox_full / ERR_MEM): {sums['mbox_full']}",
            "Mailbox counters measure Sming wake-up posts, not occupancy or network packet drops.",
            "Batch durations include synchronous callbacks and overhead; do not add them to CallbackTiming totals.",
        ])
        incidents = [(key, window) for key, window in sorted(self.windows.items())
                     if window["event_full"] or window["mbox_full"]]
        if incidents:
            lines.append("Rejection windows (boot, seq, event_full, mbox_full, sampled queue peak):")
            for (boot, sequence), window in incidents:
                lines.append(
                    f"  {boot} seq={sequence} event_full={window['event_full']} "
                    f"mbox_full={window['mbox_full']} queue_peak_sampled={window['queue_peak_sampled']}"
                )
        return "\n".join(lines)

    def plot_data(self) -> None:
        return None


def write_plot(path: pathlib.Path, data: dict[str, Any]) -> None:
    blocks = [block for module in data["modules"] for boot in module["boots"] for block in boot["blocks"]]
    if blocks:
        if any(block.get("received_at") is None for block in blocks):
            raise ValueError("Timeline requires collector receivedAt timestamps on all observed blocks")
        origin = min(block["received_at"] for block in blocks)
        data["time_origin"] = datetime.fromtimestamp(origin).astimezone().isoformat()
        for module in data["modules"]:
            for boot in module["boots"]:
                ordered = sorted(boot["blocks"], key=lambda block: block["received_at"])
                for index, block in enumerate(ordered):
                    end = block["received_at"] - origin
                    previous = ordered[index - 1] if index else None
                    # Reports describe the preceding interval; never derive the axis from device clocks.
                    start = (previous["end"] if previous is not None and block["seq"] == previous["seq"] + 1
                             else max(0, end - 5))
                    block["start"], block["end"] = start, end
                    block["gap_start"] = previous["end"] if previous is not None else start
                    block["estimated"] = previous is None or block["seq"] != previous["seq"] + 1
                boot["blocks"] = ordered
                boot["duration"] = max((block["end"] for block in ordered), default=0)
    template = pathlib.Path(__file__).with_name("log_statistics_plot.html").read_text(encoding="utf-8")
    encoded = json.dumps(data, allow_nan=False).replace("<", "\\u003c")
    path.write_text(template.replace("__PLOT_DATA__", encoded), encoding="utf-8")


STATISTICS: tuple[type[Statistic], ...] = (CallbackTiming, DispatcherTiming)
SORT_COLUMNS = ("function", "calls", "average", "max", "total", "calls/s", "ms/s", "included-in", "tree")
TIMELINE_METRICS = ("load", "total", "calls", "calls/s", "ms/s", "all")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, action="append",
                        help="controller hostname or syslog source IP; repeat to process hosts in order")
    parser.add_argument("--log", required=True, type=log_service_url,
                        help="log service hostname/address[:port] (default port: 4821)")
    for statistic in STATISTICS:
        parser.add_argument(f"--{statistic.cli_option}", action="store_true",
                            help=f"report {statistic.search_term} statistics (default: all modules)")
    parser.add_argument("-b", "--boot", type=int, metavar="OFFSET",
                        help="0: current boot, -1: previous, -2: two boots ago (default: all retained boots)")
    parser.add_argument("--sort", choices=SORT_COLUMNS, default="average",
                        help="sort by column: text ascending, numeric columns descending; "
                             "tree: indented runtime paths (default: average)")
    parser.add_argument("--timeline", nargs="?", choices=TIMELINE_METRICS, const="load",
                        help="interactive HTML plot: load percentage, total ms/block, calls/block, calls/s, ms/s, or all "
                             "(without a metric: load; default: no graph)")
    parser.add_argument("--plot", type=pathlib.Path, metavar="FILE",
                        help="HTML plot output path (default with --timeline: log-statistics.html; "
                             "--plot alone enables all metrics)")
    args = parser.parse_args(argv)
    if args.boot is not None and args.boot > 0:
        parser.error("--boot must be 0 (current) or negative (previous boots)")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        selected = [
            statistic for statistic in STATISTICS
            if getattr(args, statistic.cli_option.replace("-", "_"))
        ]
        service = LogService(args.log)
        modules = []
        controllers = []
        for host in args.host:
            ip = controller_ip(host)
            controller = f"{host} ({ip})"
            controllers.append(controller)
            statistics = [statistic() for statistic in (selected or STATISTICS)]
            boot = service.select_boot(ip, args.boot) if args.boot is not None else None
            count = 0
            terms = tuple(dict.fromkeys(statistic.search_term for statistic in statistics))
            for record in service.logs(ip, terms, boot):
                count += 1
                for statistic in statistics:
                    statistic.consume(record)
            print(f"Controller: {controller}; log service: {args.log}; matching logs read: {count}")
            if boot is not None:
                print(f"Selected boot: {boot.number} (offset {args.boot}); log IDs: {boot.first_id}..{boot.last_id}")
            for statistic in statistics:
                print()
                print(statistic.report(args.sort, args.timeline))
                if args.timeline is not None or args.plot is not None:
                    data = statistic.plot_data()
                    if data is not None:
                        modules.append({**data, "controller": controller})
        if args.timeline is not None or args.plot is not None:
            path = args.plot or pathlib.Path("log-statistics.html")
            if not modules:
                raise ValueError("Selected modules have no timeline plots; include --callback-timing")
            write_plot(path, {
                "controller": ", ".join(controllers),
                "metric": args.timeline or "all",
                "modules": modules,
            })
            print(f"\nInteractive plot: {path.resolve().as_uri()}")
        return 0
    except (OSError, ValueError, urllib.error.URLError) as err:
        print(f"Error: {err}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
