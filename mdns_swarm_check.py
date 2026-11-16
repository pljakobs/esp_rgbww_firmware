#!/usr/bin/env python3
"""Staged mDNS neighbour-scaling check for a single Host-emulator controller.

Advertises a fleet of synthetic Lightinator controllers (VirtualController) in
cumulative stages — 16, 32, 48, 64 by default — and after each stage verifies
that the device-under-test converges on exactly that many ONLINE (state==3)
neighbours within a bounded time. Records are kept alive by periodic
re-announcement so they never expire mid-stage.

Alongside the fleet, two edge-case devices are advertised for the whole run:
  * a foreign device (VirtualForeignDevice) that only speaks _http._tcp plus a
    rogue _lightinator._tcp record whose "id" TXT value overflows uint32 — it
    must NEVER appear in the inventory (service gating + strict id parsing);
  * a controller whose chip id exceeds INT_MAX — it must appear with its EXACT
    id, proving /hosts does not clamp 32-bit ids to 2147483647.

This is the ONLY HTTP client against the emulator, so it never contends with the
heavy /info call that makes the pytest harness time out. It is the committed,
parameterized successor to the throwaway /tmp/mdns_ramp_poll.py poller.

Environment overrides:
  MDNS_BASE_URL          emulator base URL            (default http://192.168.13.2)
  MDNS_INTERFACE_IP      local mDNS source interface  (default 192.168.13.1)
  MDNS_IP_START          first advertised controller  (default 192.168.13.100)
  MDNS_BASE_ID           first synthetic chip id       (default 1000900)
  MDNS_STAGES            cumulative counts             (default 16,32,48,64)
  MDNS_TTL               advertised record TTL (secs)  (default 30)
  MDNS_CONVERGE_TIMEOUT  per-stage settle budget (s)   (default 180)
  MDNS_POLL_EVERY        /hosts poll interval (secs)   (default 5)
  MDNS_HOLD             final hold/accounting window   (default 30)
  MDNS_STRICT           1 => non-convergence fails     (default 1)
  MDNS_FOREIGN_IP       foreign-device address         (default 192.168.13.90)
  MDNS_BIGID_IP         uint32-id controller address   (default 192.168.13.91)
  MDNS_BIG_ID           uint32-id controller chip id   (default 4031650001)

Exit code: 0 if every stage converged (or MDNS_STRICT=0); 1 otherwise.
"""
from __future__ import annotations

import json
import os
import sys
import time
import urllib.request
from datetime import datetime

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, REPO_ROOT)
from virtual_controllers import VirtualController, VirtualForeignDevice, controller_ip  # noqa: E402
from zeroconf import IPVersion, Zeroconf  # noqa: E402

BASE_URL = os.environ.get("MDNS_BASE_URL", "http://192.168.13.2")
INTERFACE_IP = os.environ.get("MDNS_INTERFACE_IP", "192.168.13.1")
IP_START = os.environ.get("MDNS_IP_START", "192.168.13.100")
BASE_ID = int(os.environ.get("MDNS_BASE_ID", "1000900"))
TTL = int(os.environ.get("MDNS_TTL", "30"))
CONVERGE_TIMEOUT = float(os.environ.get("MDNS_CONVERGE_TIMEOUT", "180"))
POLL_EVERY = float(os.environ.get("MDNS_POLL_EVERY", "5"))
HOLD = float(os.environ.get("MDNS_HOLD", "30"))
STRICT = os.environ.get("MDNS_STRICT", "1") != "0"
FOREIGN_IP = os.environ.get("MDNS_FOREIGN_IP", "192.168.13.90")
BIGID_IP = os.environ.get("MDNS_BIGID_IP", "192.168.13.91")
BIG_ID = int(os.environ.get("MDNS_BIG_ID", "4031650001"))
assert BIG_ID > 2**31 - 1, "MDNS_BIG_ID must exceed INT_MAX to exercise the uint32 round-trip"
REFRESH_EVERY = TTL / 3.0
ONLINE = 3  # ControllerState::ONLINE

REPORT = os.environ.get("MDNS_REPORT", os.path.join(REPO_ROOT, "out/host-swarm/summary.txt"))


def parse_stages(raw: str) -> list[int]:
    stages = [int(part) for part in raw.split(",") if part.strip()]
    if not stages or any(s <= 0 for s in stages):
        raise ValueError(f"invalid MDNS_STAGES {raw!r}: need positive integers")
    if stages != sorted(stages) or len(set(stages)) != len(stages):
        raise ValueError(f"invalid MDNS_STAGES {raw!r}: must be strictly increasing")
    return stages


STAGES = parse_stages(os.environ.get("MDNS_STAGES", "16,32,48,64"))


def now() -> str:
    return datetime.now().strftime("%H:%M:%S")


def get_hosts(all_hosts: bool = False) -> list[dict]:
    url = f"{BASE_URL}/hosts" + ("?all=true" if all_hosts else "")
    with urllib.request.urlopen(url, timeout=4) as resp:
        return json.loads(resp.read().decode()).get("hosts", [])


def accounting(expected_ids: set[int], foreign_device: VirtualForeignDevice):
    """Return (online, present_ids, corrupt, state_breakdown, foreign, bigid_online) from /hosts?all=true."""
    try:
        hosts = get_hosts(all_hosts=True)
    except Exception:
        return (-1, None, 0, {}, 0, False)
    mine = [h for h in hosts if int(h["id"]) in expected_ids]
    present = sorted(int(h["id"]) for h in mine)
    online = sum(1 for h in mine if h.get("state") == ONLINE)
    corrupt = sum(1 for h in hosts if any(ord(c) < 32 for c in h.get("hostname", "")))
    foreign = sum(1 for h in hosts
                  if h.get("ip_address") == foreign_device.ip_address
                  or foreign_device.name in str(h.get("hostname", "")))
    # The uint32-id controller must be present with its EXACT id (a clamped
    # record would carry 2147483647 and never match BIG_ID).
    bigid_online = any(int(h["id"]) == BIG_ID and h.get("state") == ONLINE for h in hosts)
    states: dict = {}
    for h in mine:
        states[h.get("state")] = states.get(h.get("state"), 0) + 1
    return (online, present, corrupt, states, foreign, bigid_online)


def main() -> int:
    total = STAGES[-1]
    fleet = [VirtualController(i, controller_ip(IP_START, i), base_chip_id=BASE_ID) for i in range(total)]
    foreign_device = VirtualForeignDevice(FOREIGN_IP)
    bigid_controller = VirtualController(0, BIGID_IP, base_chip_id=BIG_ID, hostname="virt-uint32")
    zc = Zeroconf(interfaces=[INTERFACE_IP], ip_version=IPVersion.V4Only)
    registered: list = []
    last_refresh = 0.0
    results: list[tuple[int, bool, float]] = []
    foreign_seen = 0
    bigid_ok = False

    def refresh() -> None:
        nonlocal last_refresh
        t = time.monotonic()
        if registered and t - last_refresh >= REFRESH_EVERY:
            last_refresh = t
            for svc in registered:
                try:
                    zc.update_service(svc)
                except Exception:
                    pass

    try:
        # Edge-case devices stay advertised for the whole run: the foreign
        # device (incl. a rogue swarm record with an overflowing id) must never
        # be admitted; the uint32-id controller must appear with its exact id.
        for svc in foreign_device.build_services(include_rogue_swarm=True) + bigid_controller.build_services():
            svc.host_ttl = TTL
            svc.other_ttl = TTL
            zc.register_service(svc, ttl=TTL)
            registered.append(svc)
        print(f"{now()} === advertising edge cases: foreign device at {FOREIGN_IP}, uint32-id controller "
              f"(id {BIG_ID}) at {BIGID_IP} ===", flush=True)

        fleet_advertised = 0
        for stage in STAGES:
            expected = {BASE_ID + i for i in range(stage)}
            for ctrl in fleet[fleet_advertised:stage]:
                for svc in ctrl.build_services():
                    svc.host_ttl = TTL
                    svc.other_ttl = TTL
                    zc.register_service(svc, ttl=TTL)
                    registered.append(svc)
            fleet_advertised = stage
            print(f"{now()} === advertising {stage} controllers (IDs {BASE_ID}..{BASE_ID + stage - 1}) ===", flush=True)

            started = time.monotonic()
            deadline = started + CONVERGE_TIMEOUT
            converged = False
            next_poll = 0.0
            while time.monotonic() < deadline:
                refresh()
                if time.monotonic() >= next_poll:
                    next_poll = time.monotonic() + POLL_EVERY
                    online, present, corrupt, states, foreign, bigid_online = accounting(expected, foreign_device)
                    if present is None:
                        print(f"{now()} stage {stage}: (emulator no response)", flush=True)
                    else:
                        foreign_seen = max(foreign_seen, foreign)
                        bigid_ok = bigid_ok or bigid_online
                        missing = sorted(expected - set(present))
                        tail = "" if not missing else f" missing[{len(missing)}]={missing[:6]}{'...' if len(missing) > 6 else ''}"
                        print(f"{now()} stage {stage}: online {online}/{stage} states={states} corrupt={corrupt} "
                              f"foreign={foreign} bigid={'ok' if bigid_online else 'pending'}{tail}", flush=True)
                        if online == stage and corrupt == 0 and foreign == 0:
                            elapsed = time.monotonic() - started
                            print(f"{now()} stage {stage}: CONVERGED {stage}/{stage} ONLINE in {elapsed:.0f}s, corrupt=0", flush=True)
                            converged = True
                            results.append((stage, True, elapsed))
                            break
                time.sleep(1)
            if not converged:
                elapsed = time.monotonic() - started
                print(f"{now()} stage {stage}: NOT converged within {CONVERGE_TIMEOUT:.0f}s", flush=True)
                results.append((stage, False, elapsed))

        if HOLD > 0:
            print(f"{now()} === holding {total}-controller fleet for {HOLD:.0f}s (final accounting) ===", flush=True)
            hold_until = time.monotonic() + HOLD
            expected_all = {BASE_ID + i for i in range(total)}
            while time.monotonic() < hold_until:
                refresh()
                online, present, corrupt, states, foreign, bigid_online = accounting(expected_all, foreign_device)
                if present is not None:
                    foreign_seen = max(foreign_seen, foreign)
                    bigid_ok = bigid_online  # final verdict: must still be ONLINE with the exact id
                n = -1 if present is None else len(present)
                print(f"{now()} hold: present {n}/{total} online={online} states={states} corrupt={corrupt} "
                      f"foreign={foreign} bigid={'ok' if bigid_online else 'MISSING'}", flush=True)
                time.sleep(POLL_EVERY)
    finally:
        zc.close()
        print(f"{now()} unregistered fleet; done.", flush=True)

    foreign_ok = foreign_seen == 0
    stages_ok = all(ok for _, ok, _ in results) and len(results) == len(STAGES)
    all_ok = stages_ok and foreign_ok and bigid_ok
    lines = ["mDNS neighbour-scaling check", f"base_id={BASE_ID} ttl={TTL}s per-stage-timeout={CONVERGE_TIMEOUT:.0f}s", ""]
    for stage, ok, elapsed in results:
        status = "CONVERGED" if ok else "FAILED"
        lines.append(f"  stage {stage:>3}: {status} ({elapsed:.0f}s)")
    lines.append(f"  edge foreign-device: {'EXCLUDED' if foreign_ok else f'ADMITTED x{foreign_seen} (FAIL)'}")
    lines.append(f"  edge uint32-id:      {'EXACT' if bigid_ok else 'MISSING or CLAMPED (FAIL)'}")
    lines.append("")
    lines.append(f"result: {'PASS' if all_ok else 'FAIL'} ({sum(ok for _, ok, _ in results)}/{len(STAGES)} stages converged)")
    summary = "\n".join(lines) + "\n"
    print(summary, flush=True)
    try:
        os.makedirs(os.path.dirname(REPORT), exist_ok=True)
        with open(REPORT, "w", encoding="utf-8") as fh:
            fh.write(summary)
    except OSError as exc:
        print(f"{now()} could not write report {REPORT}: {exc}", flush=True)

    if not all_ok and STRICT:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
