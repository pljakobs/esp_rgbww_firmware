#!/usr/bin/env python3
"""
Virtual Lightinator Controller mDNS Simulator
Simulates N virtual Lightinator controllers broadcasting mDNS services
matching mdnsHandler.cpp / mdnsHandler.h behavior.
"""

import argparse
import socket
import sys
import time
from zeroconf import IPVersion, ServiceInfo, Zeroconf

DEFAULT_WEBAPP_VERSION = "v2.1.0"
BASE_CHIP_ID = 1000100


class VirtualController:
    def __init__(self, index: int, ip_address: str, webapp_ver: str, is_leader: bool = False):
        self.index = index
        self.chip_id = BASE_CHIP_ID + index
        self.hostname_raw = f"lightinator-{self.chip_id}"
        self.hostname_fqdn = f"{self.hostname_raw}.local."
        self.ip_address = ip_address
        self.ip_bytes = socket.inet_aton(ip_address)
        self.webapp_ver = webapp_ver
        self.is_leader = is_leader
        self.services = []

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
            "isLeader": "1" if self.is_leader else "0",
            "webapp": self.webapp_ver
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
        "-v", "--version",
        type=str,
        default=DEFAULT_WEBAPP_VERSION,
        help=f"Webapp version string to report in TXT records (default: {DEFAULT_WEBAPP_VERSION})"
    )

    args = parser.parse_args()
    target_ip = args.ip or get_default_ip()

    print(f"[+] Initializing mDNS engine on {target_ip}...")
    zc = Zeroconf(ip_version=IPVersion.V4Only)

    # Highest chip ID becomes the swarm leader (matching checkForLeadership logic)
    highest_index = args.count - 1
    controllers: list[VirtualController] = []

    try:
        print(f"[+] Spawning {args.count} virtual controllers...")
        for i in range(args.count):
            is_leader = (i == highest_index)
            ctrl = VirtualController(
                index=i,
                ip_address=target_ip,
                webapp_ver=args.version,
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
