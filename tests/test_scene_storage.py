import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SMING_HOME = os.environ.get("SMING_HOME")


@pytest.mark.skipif(not SMING_HOME, reason="requires the existing Sming Host toolchain")
def test_native_scene_storage(tmp_path):
    """Round-trip actual generated ConfigDB scene targets without network or hardware."""
    project = tmp_path / "project"
    (project / "app").mkdir(parents=True)
    for name in ("app-data.cfgdb", "defs.cfgdb"):
        shutil.copyfile(ROOT / name, project / name)
    shutil.copyfile(ROOT / "tests/scene_storage_test.cpp", project / "app/application.cpp")
    (project / "Makefile").write_text("include $(SMING_HOME)/project.mk\n")
    (project / "component.mk").write_text(
        "COMPONENT_DEPENDS := ConfigDB\n"
        "DISABLE_NETWORK := 1\n"
        "HOST_NETWORK_OPTIONS := --nonet\n"
    )
    build = subprocess.run(
        ["make", "SMING_ARCH=Host", "BUILD_TYPE=scene-storage-test", "-j4"],
        cwd=project, capture_output=True, text=True, timeout=300,
    )
    assert build.returncode == 0, build.stdout + build.stderr
    executable = project / "out/Host/scene-storage-test/firmware/app"
    run = subprocess.run(
        [str(executable), "--nonet", f"--flashfile={tmp_path / 'flash.bin'}"],
        cwd=project, capture_output=True, text=True, timeout=30,
    )
    assert run.returncode == 0, run.stdout + run.stderr
    assert "PASS: scene targets persist, reload and export/import unchanged" in run.stdout + run.stderr
    stored = json.loads((project / "scene-data/scenes.json").read_text())
    copied = json.loads((project / "scene-copy/scenes.json").read_text())
    assert copied == stored
    steps = stored[0]["settings"]
    assert steps[0]["light"] == -1
    assert steps[0]["light_id"] in (None, "")
    assert steps[0]["color"]["hsv"]["ct"] == 2700
    assert steps[1]["light"] == 12
    assert steps[1]["light_id"] == "remote-light"
    assert steps[1]["color"]["raw"] == {"r": 1, "g": 2, "b": 3, "ww": 4, "cw": 5}
    assert steps[2]["light"] == 8
    assert steps[2]["light_id"] == "other-light"
    assert steps[2]["color"] == {"Preset": {"id": "remote-preset"}}
    presets = json.loads((project / "scene-data/presets.json").read_text())
    assert presets[0]["favorite"] is True
    assert presets[0]["light_favorites"] == []
    assert presets[1]["light_type"] == "WWCW"
    assert presets[1]["raw_channels"] == [250, 750]
    assert presets[1]["light_favorites"] == [
        {"light_id": "uuid-one", "favorite": True},
        {"light_id": "uuid-two", "favorite": False},
    ]
