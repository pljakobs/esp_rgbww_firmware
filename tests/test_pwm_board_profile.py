import subprocess
import os
import shutil
from pathlib import Path

import pytest


def test_selected_board_profile(tmp_path):
    repository = Path(__file__).resolve().parents[1]
    executable = tmp_path / "board-profile"
    subprocess.run(
        [
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I", str(repository / "include"),
            str(repository / "tests/pwm_board_profile_test.cpp"),
            "-o", str(executable),
        ],
        check=True, capture_output=True, text=True,
    )
    subprocess.run([str(executable)], check=True)


@pytest.mark.skipif(not os.environ.get("SMING_HOME"), reason="requires Sming Host toolchain")
def test_configdb_board_profile_persistence(tmp_path):
    repository = Path(__file__).resolve().parents[1]
    project = tmp_path / "project"
    (project / "app").mkdir(parents=True)
    (project / "include").mkdir()
    for name in ("app-config.cfgdb", "defs.cfgdb"):
        shutil.copyfile(repository / name, project / name)
    shutil.copyfile(repository / "include/pwmboardprofile.h", project / "include/pwmboardprofile.h")
    shutil.copyfile(repository / "tests/board_profile_storage_test.cpp", project / "app/application.cpp")
    (project / "Makefile").write_text("include $(SMING_HOME)/project.mk\n")
    (project / "component.mk").write_text(
        "COMPONENT_DEPENDS := ConfigDB\nDISABLE_NETWORK := 1\nHOST_NETWORK_OPTIONS := --nonet\n"
    )
    build = subprocess.run(
        ["make", "SMING_ARCH=Host", "BUILD_TYPE=board-profile-test", "-j4"],
        cwd=project, capture_output=True, text=True, timeout=300,
    )
    assert build.returncode == 0, build.stdout + build.stderr
    executable = project / "out/Host/board-profile-test/firmware/app"
    run = subprocess.run(
        [str(executable), "--nonet", f"--flashfile={tmp_path / 'flash.bin'}"],
        cwd=project, capture_output=True, text=True, timeout=30,
    )
    assert run.returncode == 0, run.stdout + run.stderr
    assert "PASS: selected board profile persists and reloads through ConfigDB" in run.stdout + run.stderr
