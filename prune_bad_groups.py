#!/usr/bin/env python3
"""Prune malformed groups across a fleet of RGBWW controllers.

Discovers visible controllers from a seed device's ``/hosts`` endpoint, then for
each host fetches ``/data`` and deletes every group that is malformed, i.e. has
no name or no controllers. Deletion uses the ConfigDB selector syntax
``{"groups[ts=<ts>]": []}`` POSTed back to the same host's ``/data`` endpoint.

Default is a dry run; pass --apply to actually delete.
"""

from __future__ import annotations

import argparse
import http.client
import json
import sys
import time
import urllib.error
import urllib.request

# Low heap on the device can abort the chunked /data stream mid-flight, which
# surfaces as IncompleteRead or a short read. Retry such truncations.
_GET_RETRIES = 6
_GET_BACKOFF = 1.5


def http_get_json(url: str, timeout: float) -> object:
    last_exc: Exception | None = None
    for attempt in range(_GET_RETRIES):
        try:
            req = urllib.request.Request(url, method="GET")
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                raw = resp.read()
            return json.loads(raw.decode("utf-8"))
        except (http.client.IncompleteRead, json.JSONDecodeError,
                ConnectionError, TimeoutError) as exc:
            last_exc = exc
        except urllib.error.URLError as exc:
            # Unwrap socket-level truncations/timeouts hidden inside URLError.
            reason = getattr(exc, "reason", exc)
            if not isinstance(reason, (http.client.IncompleteRead,
                                       ConnectionError, TimeoutError)):
                raise
            last_exc = exc
        if attempt < _GET_RETRIES - 1:
            time.sleep(_GET_BACKOFF * (attempt + 1))
    raise last_exc  # type: ignore[misc]


def http_post_json(url: str, body: dict, timeout: float) -> tuple[int, str]:
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, method="POST", headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", "replace")


def discover_hosts(seed: str, timeout: float, include_all: bool) -> list[dict]:
    url = f"http://{seed}/hosts" + ("?all=true" if include_all else "")
    payload = http_get_json(url, timeout)
    hosts = payload.get("hosts", []) if isinstance(payload, dict) else []
    # Only controllers with a usable IP; honour the visible flag unless --all.
    result = []
    for row in hosts:
        if not isinstance(row, dict):
            continue
        ip = row.get("ip_address")
        if not ip:
            continue
        if not include_all and not row.get("visible", False):
            continue
        result.append(row)
    return result


def _is_empty(value: object) -> bool:
    """A name/controller value counts as 'null' when missing, None or empty."""
    if value is None:
        return True
    if isinstance(value, str):
        return value.strip() == ""
    if isinstance(value, (list, dict)):
        return len(value) == 0
    return False


def _controllers_all_null(group: dict) -> bool:
    cids = group.get("controller_ids")
    if _is_empty(cids):
        return True
    if isinstance(cids, list):
        return all(_is_empty(cid) for cid in cids)
    return False


def is_bad_group(group: dict) -> bool:
    return _is_empty(group.get("name")) or _controllers_all_null(group)


def bad_groups(data: object) -> list[dict]:
    groups = data.get("groups", []) if isinstance(data, dict) else []
    return [g for g in groups if isinstance(g, dict) and is_bad_group(g)]


def describe(group: dict) -> str:
    return (
        f"ts={group.get('ts')!r} id={group.get('id')!r} "
        f"name={group.get('name')!r} controller_ids={group.get('controller_ids')!r}"
    )


def prune_host(ip: str, timeout: float, apply: bool, max_iters: int) -> int:
    """Return the number of groups deleted (or that would be deleted) on this host."""
    deleted = 0
    for _ in range(max_iters):
        try:
            data = http_get_json(f"http://{ip}/data", timeout)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
                http.client.IncompleteRead, ConnectionError) as exc:
            print(f"  [{ip}] ERROR fetching /data: {exc}", file=sys.stderr)
            return deleted

        bad = bad_groups(data)
        if not bad:
            break

        target = bad[0]
        ts = target.get("ts")
        if ts is None:
            print(
                f"  [{ip}] SKIP (no ts, cannot build selector): {describe(target)}",
                file=sys.stderr,
            )
            # Skip-by-ts is impossible; stop to avoid an infinite loop.
            break

        if not apply:
            # Dry-run cannot mutate state, so report every bad group with a ts once.
            seen_ts: set = set()
            for g in bad:
                gts = g.get("ts")
                if gts is None or gts in seen_ts:
                    continue
                seen_ts.add(gts)
                selector = {f"groups[ts={gts}]": []}
                print(f"  [{ip}] would delete: {describe(g)}  -> {json.dumps(selector)}")
                deleted += 1
            break

        selector = {f"groups[ts={ts}]": []}

        status, resp = http_post_json(f"http://{ip}/data", selector, timeout)
        if 200 <= status < 300:
            print(f"  [{ip}] deleted ts={ts}: {describe(target)}")
            deleted += 1
        else:
            print(
                f"  [{ip}] FAILED delete ts={ts} (HTTP {status}): {resp.strip()}",
                file=sys.stderr,
            )
            break
    else:
        print(f"  [{ip}] WARNING: reached max-iterations ({max_iters}); giving up",
              file=sys.stderr)
    return deleted


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.29.101",
                        help="seed device to query /hosts from (default: 192.168.29.101)")
    parser.add_argument("--apply", action="store_true",
                        help="actually delete; without this the script only reports")
    parser.add_argument("--all", action="store_true",
                        help="include non-visible hosts (adds ?all=true to /hosts)")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="per-request timeout in seconds (default: 10)")
    parser.add_argument("--max-iters", type=int, default=50,
                        help="max delete iterations per host (default: 50)")
    args = parser.parse_args()

    try:
        hosts = discover_hosts(args.host, args.timeout, args.all)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        print(f"ERROR discovering hosts from {args.host}: {exc}", file=sys.stderr)
        return 1

    if not hosts:
        print(f"No hosts found via http://{args.host}/hosts")
        return 0

    mode = "APPLY" if args.apply else "DRY-RUN"
    print(f"[{mode}] {len(hosts)} host(s) from http://{args.host}/hosts")

    total = 0
    for row in hosts:
        ip = row["ip_address"]
        name = row.get("hostname", "").rstrip(".")
        print(f"- {ip} ({name})")
        total += prune_host(ip, args.timeout, args.apply, args.max_iters)

    verb = "deleted" if args.apply else "would delete"
    print(f"\n{mode}: {verb} {total} malformed group(s) across {len(hosts)} host(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
