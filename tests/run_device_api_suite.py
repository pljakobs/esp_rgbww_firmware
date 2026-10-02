"""Run the HTTP/WebSocket API suite against a device with log-service capture.

Usage: python3 tests/run_device_api_suite.py [--device-ip 192.168.29.62] [pytest options]
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--device-ip",
        default=os.environ.get("HOST_SMOKE_APP_IP", "192.168.29.62"),
        help="IPv4 address of the ESP8266",
    )
    args, pytest_args = parser.parse_known_args()

    device_ip = args.device_ip
    base_url = f"http://{device_ip}"
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    env = os.environ.copy()
    env.update(
        {
            "HOST_SMOKE_APP_IP": device_ip,
            "HOST_SMOKE_BASE_URL": base_url,
            "HOST_SMOKE_INFO_URL": f"{base_url}/info",
            "HOST_SMOKE_COLOR_URL": f"{base_url}/color",
            "HOST_SMOKE_WS_HOST": device_ip,
            "HOST_SMOKE_WS_PORT": "80",
            "HOST_SMOKE_WS_PATH": "/ws",
            "HOST_SMOKE_LOG_SERVICE_PORT": os.environ.get("HOST_SMOKE_LOG_SERVICE_PORT", "4821"),
            "HOST_SMOKE_LOG_DIR": os.environ.get(
                "HOST_SMOKE_LOG_DIR", f"/tmp/esp-rgbww-device-api-{device_ip}-{run_id}"
            ),
            "HOST_SMOKE_REAL_DEVICE": "1",
            "HOST_SMOKE_CAPTURE_LOG_SERVICE": "1",
        }
    )

    repo_root = Path(__file__).resolve().parents[1]
    command = [
        sys.executable,
        "-m",
        "pytest",
        "-s",
        "-v",
        "tests/host_smoke_api_test.py",
        *pytest_args,
    ]
    return subprocess.run(command, cwd=repo_root, env=env, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())