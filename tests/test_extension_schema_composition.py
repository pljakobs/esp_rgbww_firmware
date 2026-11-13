from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("extension_schema", ROOT / "tools/configdb-with-extensions.py")
assert SPEC is not None and SPEC.loader is not None
schema = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(schema)


def variant(name: str) -> dict:
	return {"title": name, "type": "object", "properties": {"value": {"type": "integer"}}}


def write_json(path: Path, value) -> None:
	path.parent.mkdir(parents=True, exist_ok=True)
	path.write_text(json.dumps(value), encoding="utf-8")


def test_discovers_and_splices_variants_in_stable_order(tmp_path: Path) -> None:
	"""Only direct extension fragments are appended in deterministic path order."""
	core = tmp_path / "jsonrpc.cfgdb"
	extensions = tmp_path / "extensions"
	write_json(core, {"oneOf": [variant("core")]})
	write_json(extensions / "z" / "protocol.json", [variant("zResult"), variant("zCommand")])
	write_json(extensions / "a" / "protocol.json", [variant("aResult")])
	write_json(extensions / "a" / "nested" / "protocol.json", [variant("ignored")])

	result = schema.compose(core, extensions)
	assert [entry["title"] for entry in result["oneOf"]] == ["core", "aResult", "zResult", "zCommand"]
	assert json.loads(core.read_text()) == {"oneOf": [variant("core")]}


def test_addition_removal_and_unchanged_output(tmp_path: Path) -> None:
	"""Adding and removing fragments changes output, but identical output is not rewritten."""
	core = tmp_path / "jsonrpc.cfgdb"
	extensions = tmp_path / "extensions"
	output = tmp_path / "generated/jsonrpc.cfgdb"
	fragment = extensions / "sensor/protocol.json"
	write_json(core, {"oneOf": [variant("core")]})
	schema.write_if_changed(output, schema.compose(core, extensions))
	before = output.stat().st_mtime_ns
	schema.write_if_changed(output, schema.compose(core, extensions))
	assert output.stat().st_mtime_ns == before
	write_json(fragment, [variant("sensor")])
	schema.write_if_changed(output, schema.compose(core, extensions))
	assert len(json.loads(output.read_text())["oneOf"]) == 2
	fragment.unlink()
	schema.write_if_changed(output, schema.compose(core, extensions))
	assert json.loads(output.read_text()) == {"oneOf": [variant("core")]}


@pytest.mark.parametrize(
	"fragment, message",
	[
		({}, "must be an array"),
		(["invalid"], "must be an object"),
		([{"type": "object"}], "needs a nonempty title"),
		([variant("base_name")], "duplicate variant"),
	],
)
def test_rejects_bad_fragments(tmp_path: Path, fragment, message: str) -> None:
	"""Malformed or conflicting protocol variants are rejected explicitly."""
	core = tmp_path / "jsonrpc.cfgdb"
	extensions = tmp_path / "extensions"
	write_json(core, {"oneOf": [variant("base-name")]})
	write_json(extensions / "sensor/protocol.json", fragment)
	with pytest.raises(ValueError, match=message):
		schema.compose(core, extensions)


def test_refs_and_duplicate_extensions(tmp_path: Path) -> None:
	"""Reference names and duplicates across extensions use ConfigDB identifiers."""
	core = tmp_path / "jsonrpc.cfgdb"
	extensions = tmp_path / "extensions"
	write_json(core, {"oneOf": [{"$ref": "params/$defs/core"}]})
	write_json(extensions / "a/protocol.json", [variant("sensor-result")])
	write_json(extensions / "b/protocol.json", [variant("sensorResult")])
	with pytest.raises(ValueError, match="duplicate variant"):
		schema.compose(core, extensions)


def test_request_parameters_are_spliced_and_removed(tmp_path: Path) -> None:
	core = tmp_path / "params.cfgdb"
	extensions = tmp_path / "extensions"
	fragment = extensions / "sensor/request-params.json"
	write_json(core, {"$defs": {"ws-request-params": {"properties": {"cmd": {"type": "string"}}}}})
	write_json(fragment, {"extension_id": {"type": "string"}})
	props = schema.compose_request_params(core, extensions)["$defs"]["ws-request-params"]["properties"]
	assert props == {"cmd": {"type": "string"}, "extension_id": {"type": "string"}}
	assert "extension_id" not in json.loads(core.read_text())["$defs"]["ws-request-params"]["properties"]
	write_json(extensions / "other/request-params.json", {"extension_id": {"type": "string"}})
	assert schema.compose_request_params(core, extensions)["$defs"]["ws-request-params"]["properties"] == props
	fragment.unlink()
	(extensions / "other/request-params.json").unlink()
	assert "extension_id" not in schema.compose_request_params(core, extensions)["$defs"]["ws-request-params"]["properties"]


@pytest.mark.parametrize("fragment", [
	{"cmd": {"type": "integer"}},
	{"extension-id": {"type": "string"}, "extension_id": {"type": "integer"}},
	{"": {"type": "string"}},
	{"field": "string"},
	[],
])
def test_rejects_bad_request_parameters(tmp_path: Path, fragment) -> None:
	core = tmp_path / "params.cfgdb"
	extensions = tmp_path / "extensions"
	write_json(core, {"$defs": {"ws-request-params": {"properties": {"cmd": {"type": "string"}}}}})
	write_json(extensions / "sensor/request-params.json", fragment)
	with pytest.raises(ValueError):
		schema.compose_request_params(core, extensions)


def test_composed_protocol_generates_configdb_types(tmp_path: Path) -> None:
	"""dbgen accepts a composed protocol and generates the extension variant."""
	sming_home = os.environ.get("SMING_HOME")
	if not sming_home:
		pytest.skip("SMING_HOME is not set")
	generator = Path(sming_home) / "Libraries/ConfigDB/tools/dbgen.py"
	if not generator.is_file():
		pytest.skip("Sming ConfigDB generator is not installed")
	extensions = tmp_path / "extensions"
	write_json(extensions / "sensor/protocol.json", [variant("sensorResult")])
	generated = tmp_path / "schema/jsonrpc.cfgdb"
	schema.write_if_changed(generated, schema.compose(ROOT / "jsonrpc.cfgdb", extensions))
	inputs = [str(path) for path in sorted(ROOT.glob("*.cfgdb")) if path.name != "jsonrpc.cfgdb"]
	env = os.environ.copy()
	env["SMING_RELEASE"] = env.get("SMING_RELEASE", "0")
	python_tools = str(Path(sming_home).parent / "Tools/Python")
	env["PYTHONPATH"] = os.pathsep.join(filter(None, [python_tools, env.get("PYTHONPATH")]))
	result = subprocess.run(
		[sys.executable, str(generator), "--outdir", str(tmp_path / "out"), *inputs, str(generated)],
		cwd=ROOT,
		env=env,
		capture_output=True,
		text=True,
		check=False,
	)
	assert result.returncode == 0, result.stdout + result.stderr
	assert "ContainedSensorResult" in (tmp_path / "out/jsonrpc.h").read_text()


def test_as7341_protocol_contract() -> None:
	"""The AS7341 body schemas expose the trigger and complete raw channel set."""
	fragment = json.loads((ROOT / "extensions/as7341/protocol.json").read_text())
	variants = {entry["title"]: entry for entry in fragment}
	assert set(variants) == {
		"extension_spectrum_start_measurement",
		"extension_spectrum_measurement_started",
		"extension_spectrum_readings",
	}
	start = variants["extension_spectrum_start_measurement"]
	assert start["required"] == ["extension_id"]
	assert set(start["properties"]) == {"extension_id"}
	started = variants["extension_spectrum_measurement_started"]
	assert set(started["required"]) == {"extension_id", "valid"}
	assert started["properties"]["valid"]["default"] is False
	result = variants["extension_spectrum_readings"]
	assert set(result["required"]) == {"extension_id", "valid", "state", "error", "age_ms", "data"}
	properties = result["properties"]
	assert properties["valid"]["default"] is False
	assert properties["state"]["enum"] == ["idle", "measuring", "complete", "error"]
	assert properties["state"]["default"] == "idle"
	assert properties["error"]["default"] == ""
	assert properties["age_ms"]["minimum"] == 0
	assert properties["age_ms"]["maximum"] == 2147483647
	data = properties["data"]
	channels = {f"f{index}" for index in range(1, 9)} | {
		"clear_low", "nir_low", "clear_high", "nir_high",
	}
	assert set(data["required"]) == channels | {"atime", "astep", "gain"}
	assert set(data["properties"]) == set(data["required"])
	for channel in channels:
		assert data["properties"][channel] == {"type": "integer", "minimum": 0, "maximum": 65535}
	assert data["properties"]["atime"]["maximum"] == 255
	assert data["properties"]["astep"]["maximum"] == 65535
	assert data["properties"]["gain"]["maximum"] == 10


def test_as7341_protocol_generates_configdb_types(tmp_path: Path) -> None:
	"""dbgen generates the AS7341 request, acknowledgement and nested result."""
	sming_home = os.environ.get("SMING_HOME")
	if not sming_home:
		pytest.skip("SMING_HOME is not set")
	generator = Path(sming_home) / "Libraries/ConfigDB/tools/dbgen.py"
	if not generator.is_file():
		pytest.skip("Sming ConfigDB generator is not installed")
	generated = tmp_path / "schema/jsonrpc.cfgdb"
	schema.write_if_changed(generated, schema.compose(ROOT / "jsonrpc.cfgdb", ROOT / "extensions"))
	generated_params = tmp_path / "schema/params.cfgdb"
	schema.write_if_changed(generated_params, schema.compose_request_params(ROOT / "params.cfgdb", ROOT / "extensions"))
	inputs = [str(path) for path in sorted(ROOT.glob("*.cfgdb")) if path.name not in ("jsonrpc.cfgdb", "params.cfgdb")]
	inputs.append(str(generated_params))
	env = os.environ.copy()
	env["SMING_RELEASE"] = env.get("SMING_RELEASE", "0")
	env["PYTHONPATH"] = os.pathsep.join(filter(None, [
		str(Path(sming_home).parent / "Tools/Python"), env.get("PYTHONPATH"),
	]))
	result = subprocess.run(
		[sys.executable, str(generator), "--outdir", str(tmp_path / "out"), *inputs, str(generated)],
		cwd=ROOT,
		env=env,
		capture_output=True,
		text=True,
		check=False,
	)
	assert result.returncode == 0, result.stdout + result.stderr
	header = (tmp_path / "out/jsonrpc.h").read_text()
	for name in ("ExtensionSpectrumStartMeasurement", "ExtensionSpectrumMeasurementStarted", "ExtensionSpectrumReadings"):
		assert f"Contained{name}" in header
		assert f"to{name}()" in header
	assert "const ContainedData data;" in header
	assert "DataUpdater data;" in header
	for name in ("F1", "F8", "ClearLow", "NirLow", "ClearHigh", "NirHigh", "Atime", "Astep", "Gain"):
		assert f"get{name}()" in header
		assert f"set{name}(" in header
	assert "getStateValue()" in header
	assert "getExtensionId()" in (tmp_path / "out/params.h").read_text()
	source = (tmp_path / "out/jsonrpc.cpp").read_text()
	for name in ("idle", "measuring", "complete", "error"):
		assert f'"{name}"' in source


def test_extension_request_params_round_trip_on_host(tmp_path: Path) -> None:
	"""The real ConfigDB request parser retains composed extension parameters."""
	if not os.environ.get("SMING_HOME"):
		pytest.skip("SMING_HOME is not set")
	project = tmp_path / "project"
	(project / "app").mkdir(parents=True)
	for path in ROOT.glob("*.cfgdb"):
		if path.name == "jsonrpc.cfgdb":
			write_json(project / path.name, schema.compose(path, ROOT / "extensions"))
		elif path.name == "params.cfgdb":
			write_json(project / path.name, schema.compose_request_params(path, ROOT / "extensions"))
		else:
			shutil.copyfile(path, project / path.name)
	shutil.copyfile(ROOT / "tests/extension_request_test.cpp", project / "app/application.cpp")
	(project / "Makefile").write_text("include $(SMING_HOME)/project.mk\n")
	(project / "component.mk").write_text(
		"COMPONENT_DEPENDS := ConfigDB\nDISABLE_NETWORK := 1\nHOST_NETWORK_OPTIONS := --nonet\n"
	)
	build = subprocess.run(
		["make", "SMING_ARCH=Host", "BUILD_TYPE=extension-request-test", "-j4"],
		cwd=project, capture_output=True, text=True, timeout=300,
	)
	assert build.returncode == 0, build.stdout + build.stderr
	executable = project / "out/Host/extension-request-test/firmware/app"
	run = subprocess.run(
		[str(executable), "--nonet", f"--flashfile={tmp_path / 'flash.bin'}"],
		cwd=project, capture_output=True, text=True, timeout=30,
	)
	assert run.returncode == 0, run.stdout + run.stderr
	assert "PASS: extension request parameters round-trip" in run.stdout + run.stderr


def test_extension_storage_schemas_are_separate_and_remove_stale_code(tmp_path: Path) -> None:
	"""Each extension gets an independent generated database, including removal."""
	extensions = tmp_path / "extensions"
	generated = tmp_path / "generated"
	out = tmp_path / "out"
	write_json(extensions / "ina219/storage.cfgdb", {"type": "object", "properties": {"enabled": {"type": "boolean"}}})
	write_json(extensions / "as7341/storage.cfgdb", {"type": "object", "properties": {"enabled": {"type": "boolean"}}})
	databases = schema.storage_schemas(extensions, generated)
	assert [name for name, _, _ in databases] == ["extension_as7341", "extension_ina219"]
	schema.remove_old_storage_outputs(out, [name for name, _, _ in databases])
	for name, _, _ in databases:
		for suffix in (".h", ".cpp"):
			(out / f"{name}{suffix}").write_text("generated")
		write_json(out / "schema" / f"{name}.json", {})
	schema.remove_old_storage_outputs(out, ["extension_ina219"])
	assert not (out / "extension_as7341.h").exists()
	assert not (out / "extension_as7341.cpp").exists()
	assert not (out / "schema/extension_as7341.json").exists()
	assert (out / "extension_ina219.h").exists()


def test_invalid_storage_extension_name_is_rejected(tmp_path: Path) -> None:
	"""Invalid extension names cannot create ambiguous generated database paths."""
	extensions = tmp_path / "extensions"
	write_json(extensions / "bad-name/storage.cfgdb", {"type": "object"})
	with pytest.raises(ValueError, match="extension directory must use"):
		schema.storage_schemas(extensions, tmp_path / "generated")
