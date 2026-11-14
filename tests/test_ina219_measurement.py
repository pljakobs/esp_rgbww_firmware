import subprocess
import os
import shutil
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_ina219_measurement(tmp_path):
    executable = tmp_path / "ina219-test"
    subprocess.run(
        ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
         str(ROOT / "tests/ina219_measurement_test.cpp"),
         str(ROOT / "extensions/ina219/measurement.cpp"), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)


@pytest.mark.skipif(not os.environ.get("SMING_HOME"), reason="requires the Sming Host toolchain")
def test_ina219_settings_persist(tmp_path):
    project = tmp_path / "project"
    (project / "app").mkdir(parents=True)
    shutil.copyfile(ROOT / "extensions/ina219/storage.cfgdb", project / "extension_ina219.cfgdb")
    shutil.copyfile(ROOT / "tests/ina219_storage_test.cpp", project / "app/application.cpp")
    (project / "Makefile").write_text("include $(SMING_HOME)/project.mk\n")
    (project / "component.mk").write_text(
        "COMPONENT_DEPENDS := ConfigDB\n"
        "DISABLE_NETWORK := 1\n"
        "HOST_NETWORK_OPTIONS := --nonet\n"
    )
    build = subprocess.run(
        ["make", "SMING_ARCH=Host", "BUILD_TYPE=ina219-storage-test", "-j4"],
        cwd=project, capture_output=True, text=True, timeout=300,
    )
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run(
        [str(project / "out/Host/ina219-storage-test/firmware/app"),
         "--nonet", f"--flashfile={tmp_path / 'flash.bin'}"],
        cwd=project, capture_output=True, text=True, timeout=30,
    )
    assert run.returncode == 0, run.stdout + run.stderr
    assert "PASS: INA219 settings persisted" in run.stdout + run.stderr
