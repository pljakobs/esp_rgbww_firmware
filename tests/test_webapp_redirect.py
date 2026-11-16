import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("arch", ["ARCH_ESP8266", "ARCH_ESP32"])
def test_webapp_leader_redirect(tmp_path, arch):
    """Run actual redirect handlers against ID boundaries and changing peer visibility."""
    compiler = shutil.which("g++") or shutil.which("c++")
    assert compiler is not None, "A C++ compiler is required"
    source = (ROOT / "app/webserver.cpp").read_text()
    start = source.index("#ifdef ARCH_ESP8266\nvoid ApplicationWebserver::updateWebappLeader(")
    end = source.index("void ApplicationWebserver::onWebappCheck(", start)
    (tmp_path / "webapp_redirect_methods.inc").write_text(source[start:end])
    executable = tmp_path / "webapp-redirect-test"
    build = subprocess.run(
        [
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", f"-D{arch}",
            "-I", str(tmp_path), str(ROOT / "tests/webapp_redirect_test.cpp"),
            "-o", str(executable),
        ],
        capture_output=True, text=True, timeout=60,
    )
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run(
        [str(executable)], capture_output=True, text=True, timeout=10,
    )
    assert run.returncode == 0, run.stdout + run.stderr


def test_webapp_browser_fallback():
    node = shutil.which("node")
    assert node is not None, "Node.js is required to test the embedded browser fallback"
    run = subprocess.run(
        [node, "--test", str(ROOT / "tests/webapp_redirect_test.mjs")],
        capture_output=True, text=True, timeout=30,
    )
    assert run.returncode == 0, run.stdout + run.stderr
