from __future__ import annotations

import os
from pathlib import Path

import pytest


_test_results: list[tuple[str, str, str, str, str]] = []


def _markdown(value: str) -> str:
	return " ".join(value.split()).replace("|", "\\|")


def _failure_detail(report) -> str:
	lines = getattr(report, "longreprtext", "").splitlines()
	details = [line.strip()[2:].strip() for line in lines if line.lstrip().startswith("E ")]
	if details:
		return "; ".join(details[:3])
	return lines[-1].strip() if lines else "Test failed without an assertion detail"


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
	outcome = yield
	report = outcome.get_result()
	if report.skipped:
		status = "SKIP"
	elif report.when == "call":
		status = "PASS" if report.passed else "FAIL"
	elif report.when == "setup" and report.failed:
		status = "FAIL"
	else:
		return

	description = (item.function.__doc__ or "").strip().splitlines()
	expected = description[0] if description else item.originalname.replace("_", " ")
	if report.passed:
		observed = "All assertions passed; expected behavior observed."
	elif report.skipped:
		observed = str(report.longrepr[2]) if isinstance(report.longrepr, tuple) else str(report.longrepr)
	else:
		observed = _failure_detail(report)
	_test_results.append((item.nodeid, item.originalname.replace("_", " "), expected, observed, status))


def pytest_sessionfinish(session, exitstatus):
	if not _test_results:
		return
	rows = [
		"| Test name | Description | Expected behavior | Observed behavior | Mark |",
		"| --- | --- | --- | --- | --- |",
	]
	rows.extend("| " + " | ".join(_markdown(value) for value in row) + " |" for row in _test_results)
	table = "\n".join(rows) + "\n"
	terminal = session.config.pluginmanager.get_plugin("terminalreporter")
	if terminal is not None:
		terminal.write_sep("=", "TEST RESULT TABLE")
		for line in rows:
			terminal.write_line(line)

	output_path = os.environ.get("PYTEST_RESULT_TABLE")
	if output_path:
		path = Path(output_path)
		path.parent.mkdir(parents=True, exist_ok=True)
		path.write_text(table, encoding="utf-8")