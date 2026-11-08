from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path


def test_pwm_channel_planner_suite() -> None:
	"""The planner splits lights across banks and rejects per-bank overflow."""
	repository = Path(__file__).resolve().parents[1]
	compiler = shutil.which("g++") or shutil.which("c++")
	assert compiler is not None, "A C++ compiler is required for PWM planner tests"

	with tempfile.TemporaryDirectory(prefix="pwm-channel-planner-") as temporary_directory:
		executable = Path(temporary_directory) / "pwm-channel-planner-test"
		compile_result = subprocess.run(
			[
				compiler,
				"-std=c++17",
				"-Wall",
				"-Wextra",
				"-Werror",
				"-I",
				str(repository / "include"),
				str(repository / "tests/pwm_channel_planner_test.cpp"),
				"-o",
				str(executable),
			],
			capture_output=True,
			text=True,
		)
		assert compile_result.returncode == 0, compile_result.stderr
		test_result = subprocess.run([str(executable)], capture_output=True, text=True)
		assert test_result.returncode == 0, test_result.stderr