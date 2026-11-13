import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_as7341_measurement_state_machine(tmp_path):
    executable = tmp_path / "as7341-test"
    subprocess.run(
        ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
         str(ROOT / "tests/as7341_measurement_test.cpp"),
         str(ROOT / "extensions/as7341/measurement.cpp"), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
