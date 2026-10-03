#!/usr/bin/env python3
"""
Virtual Lightinator Controller mDNS Simulator
Simulates N virtual Lightinator controllers broadcasting mDNS services
matching mdnsHandler.cpp / mdnsHandler.h behavior.
"""

from __future__ import annotations

import argparse
from ipaddress import IPv4Address
import socket
import time
from zeroconf import IPVersion, ServiceInfo, Zeroconf

BASE_CHIP_ID = 1000100


class VirtualController:
    def __init__(self, index: int, ip_address: str, is_leader: bool = False, base_chip_id: int = BASE_CHIP_ID,
                 hostname: str | None = None):
        self.index = index
        self.chip_id = base_chip_id + index
        self.set_hostname(hostname or f"lightinator-{self.chip_id}")
        self.set_ip(ip_address)
        self.is_leader = is_leader
        self.services = []

    def set_hostname(self, hostname: str) -> None:
        """Change the advertised hostname; the controller ID stays the same. Call build_services() afterwards."""
        self.hostname_raw = hostname
        self.hostname_fqdn = f"{hostname}.local."

    def set_ip(self, ip_address: str) -> None:
        """Change the advertised IPv4 address; the controller ID stays the same. Call build_services() afterwards."""
        self.ip_address = ip_address
        self.ip_bytes = socket.inet_aton(ip_address)

    def build_services(self) -> list[ServiceInfo]:
        """Constructs the 3 mDNS services per controller matching mdnsHandler.cpp"""
        
        # 1. _lightinator-api._tcp (Machine-readable REST API endpoint)
        api_txt = {
            "mo": "esp8266",
            "id": str(self.chip_id),
            "fn": self.hostname_raw,
            "type": "CONTROLLER",
            "host_type": "CONTROLLER",
            "path": "/",
            "v": "2"
        }
        api_service = ServiceInfo(
            "_lightinator-api._tcp.local.",
            f"{self.hostname_raw}._lightinator-api._tcp.local.",
            addresses=[self.ip_bytes],
            port=80,
            properties=api_txt,
            server=self.hostname_fqdn
        )

        # 2. _lightinator._tcp (Controller-to-controller swarm gossip)
        swarm_txt = {
            "id": str(self.chip_id),
            "type": "CONTROLLER",
            "host_type": "CONTROLLER",
            "isLeader": "1" if self.is_leader else "0"
        }
        swarm_service = ServiceInfo(
            "_lightinator._tcp.local.",
            f"{self.hostname_raw}._lightinator._tcp.local.",
            addresses=[self.ip_bytes],
            port=80,
            properties=swarm_txt,
            server=self.hostname_fqdn
        )

        # 3. _http._tcp (Human-facing web frontend)
        http_txt = {
            "fn": self.hostname_raw,
            "id": str(self.chip_id),
            "type": "CONTROLLER",
            "host_type": "CONTROLLER"
        }
        http_service = ServiceInfo(
            "_http._tcp.local.",
            f"{self.hostname_raw}._http._tcp.local.",
            addresses=[self.ip_bytes],
            port=80,
            properties=http_txt,
            server=self.hostname_fqdn
        )

        self.services = [api_service, swarm_service, http_service]
        return self.services


def controller_ip(ip_start: str, index: int) -> str:
    if index < 0:
        raise ValueError("controller index must be non-negative")
    return str(IPv4Address(int(IPv4Address(ip_start)) + index))


def get_default_ip() -> str:
    """Detects primary local IP address."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
    except Exception:
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip


def main():
    parser = argparse.ArgumentParser(description="Simulate Lightinator mDNS Virtual Controllers")
    parser.add_argument(
        "-n", "--count",
        type=int,
        default=3,
        help="Number of virtual controllers to spawn (default: 3)"
    )
    parser.add_argument(
        "-i", "--ip",
        type=str,
        default=None,
        help="IP address to advertise (defaults to primary local interface IP)"
    )
    parser.add_argument(
        "--ip-start",
        help="First advertised IPv4 address; subsequent controllers use successive addresses"
    )
    parser.add_argument(
        "--interface-ip",
        help="Local IPv4 interface used to send mDNS (for example 192.168.13.1)"
    )

    args = parser.parse_args()
    if args.count < 1:
        parser.error("count must be positive")
    if args.ip_start:
        try:
            controller_ip(args.ip_start, args.count - 1)
        except ValueError as exc:
            parser.error(str(exc))
    target_ip = args.ip or get_default_ip()

    print(f"[+] Initializing mDNS engine on {target_ip}...")
    interfaces = {"interfaces": [args.interface_ip]} if args.interface_ip else {}
    zc = Zeroconf(ip_version=IPVersion.V4Only, **interfaces)

    # Highest chip ID becomes the swarm leader (matching checkForLeadership logic)
    highest_index = args.count - 1
    controllers: list[VirtualController] = []

    try:
        print(f"[+] Spawning {args.count} virtual controllers...")
        for i in range(args.count):
            is_leader = (i == highest_index)
            ctrl = VirtualController(
                index=i,
                ip_address=controller_ip(args.ip_start, i) if args.ip_start else target_ip,
                is_leader=is_leader
            )
            controllers.append(ctrl)

            for srv in ctrl.build_services():
                zc.register_service(srv)
            time.sleep(0.1)

            leader_str = " [SWARM LEADER]" if is_leader else ""
            print(f"  └─ Spawned '{ctrl.hostname_raw}' (ID: {ctrl.chip_id}){leader_str}")

        print("\n[+] All virtual controllers active. Listening for mDNS queries (Ctrl+C to stop)...")
        while True:
            time.sleep(1)

    except KeyboardInterrupt:
        print("\n[-] Shutting down virtual controllers and unregistering mDNS services...")
    finally:
        zc.close()
        print("[+] Cleanup complete.")


if __name__ == "__main__":
    main()
