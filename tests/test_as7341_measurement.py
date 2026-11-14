import subprocess
import os
import shutil
from pathlib import Path
import pytest


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


@pytest.mark.skipif(not os.environ.get("SMING_HOME"), reason="requires the Sming Host toolchain")
def test_as7341_settings_persist(tmp_path):
    project = tmp_path / "project"
    (project / "app").mkdir(parents=True)
    shutil.copyfile(ROOT / "extensions/as7341/storage.cfgdb", project / "extension_as7341.cfgdb")
    shutil.copyfile(ROOT / "tests/as7341_settings_test.cpp", project / "app/application.cpp")
    for name in ["as7341_extension.cpp", "as7341_extension.h", "measurement.cpp", "measurement.h"]:
        shutil.copyfile(ROOT / "extensions/as7341" / name, project / "app" / name)
    (project / "Makefile").write_text("include $(SMING_HOME)/project.mk\n")
    (project / "component.mk").write_text(
        "COMPONENT_DEPENDS := ConfigDB\n"
        "DISABLE_NETWORK := 1\n"
        "HOST_NETWORK_OPTIONS := --nonet\n"
    )
    build = subprocess.run(
        ["make", "SMING_ARCH=Host", "BUILD_TYPE=as7341-settings-test", "-j4"],
        cwd=project, capture_output=True, text=True, timeout=300,
    )
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run(
        [str(project / "out/Host/as7341-settings-test/firmware/app"),
         "--nonet", f"--flashfile={tmp_path / 'flash.bin'}"],
        cwd=project, capture_output=True, text=True, timeout=30,
    )
    assert run.returncode == 0, run.stdout + run.stderr
    assert "PASS: AS7341 settings persisted" in run.stdout + run.stderr
