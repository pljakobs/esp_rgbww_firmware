import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const schema = JSON.parse(readFileSync(resolve(root, "app-config.cfgdb"), "utf8"));
const hardware = schema.properties.hardware;
const defaults = JSON.parse(readFileSync(resolve(root, "hardware-defaults.json"), "utf8"));

test("5.1 extends available_pins and replaces fixed pinconfigs", () => {
  assert.equal(hardware.properties.schema_version.default, 2);
  assert.equal(hardware.properties.available_pins.items.properties.pins.items.type, "object");
  assert.ok(hardware.properties.templates);
  assert.ok(hardware.properties.boards);
  assert.ok(hardware.properties.configuration);
  assert.equal(hardware.properties.pinconfigs, undefined);
  assert.deepEqual(defaults.available_pins.map(entry => entry.soc), ["esp32", "esp32c3"]);
});

test("built-in board output mappings match the vendor pinouts", () => {
  const expected = {
    "quinled-an-penta-mini": [5, 4, 1, 0, 3],
    "quinled-an-penta-deca": [27, 26, 25, 23, 22, 21, 19, 18, 17, 14, 13, 12, 5, 4, 2],
    "quinled-an-penta-plus": [33, 32, 12, 4, 2],
  };
  for (const board of defaults.boards) {
    const pins = board.implemented_pins.filter(pin => pin.allowed_functions.includes("light.pwm"));
    assert.deepEqual(pins.map(pin => pin.gpio), expected[board.id]);
    assert.equal(new Set(board.implemented_pins.map(pin => pin.id)).size, board.implemented_pins.length);
    assert.equal(new Set(board.implemented_pins.map(pin => pin.gpio)).size, board.implemented_pins.length);
    const capabilities = new Map(defaults.available_pins.find(entry => entry.soc === board.soc).pins.map(pin => [pin.gpio, pin]));
    for (const pin of board.implemented_pins) {
      assert.ok(capabilities.has(pin.gpio));
      assert.ok(!capabilities.get(pin.gpio).reserved);
      if (pin.allowed_functions.includes("light.pwm")) {
        assert.equal(capabilities.get(pin.gpio).pwm, true);
        assert.ok(!board.reserved_gpios?.includes(pin.gpio));
      }
    }
  }
});

test("SDA/SCL and buttons are physical board resources", () => {
  for (const board of defaults.boards) {
    const pins = new Map(board.implemented_pins.map(pin => [pin.id, pin]));
    const mini = board.soc === "esp32c3";
    assert.equal(pins.get("I2C_SDA").gpio, mini ? 6 : 15);
    assert.equal(pins.get("I2C_SCL").gpio, mini ? 7 : 16);
    assert.deepEqual([1, 2, 3].map(number => pins.get(`Button_${number}`).gpio), mini ? [2, 8, 9] : [36, 39, 34]);
    for (const number of [1, 2, 3]) {
      assert.equal(pins.get(`Button_${number}`).external_pull, "up");
      assert.equal(pins.get(`Button_${number}`).hardware_debounce, true);
    }
  }
});

test("templates resolve to unique PWM outputs without enabling peripherals", () => {
  for (const template of defaults.templates) {
    const board = defaults.boards.find(entry => entry.id === template.board_id);
    assert.equal(template.board_revision, board.revision);
    assert.equal(template.lights.length, 1);
    const pins = new Map(board.implemented_pins.map(pin => [pin.id, pin]));
    const outputs = template.lights[0].outputs;
    assert.deepEqual(Object.keys(outputs), ["red", "green", "blue", "warmwhite", "coldwhite"]);
    assert.equal(new Set(Object.values(outputs)).size, 5);
    for (const id of Object.values(outputs)) {
      assert.ok(pins.get(id).allowed_functions.includes("light.pwm"));
    }
    assert.equal(template.system.clear_config_pin, "");
    for (const bus of template.buses) {
      assert.equal(bus.enabled, false);
      assert.ok(pins.get(bus.sda).allowed_functions.includes("i2c.sda"));
      assert.ok(pins.get(bus.scl).allowed_functions.includes("i2c.scl"));
    }
    const miniOrPlus = board.id !== "quinled-an-penta-deca";
    assert.equal(outputs.red, miniOrPlus ? "L5" : "L1");
    assert.equal(outputs.coldwhite, miniOrPlus ? "L1" : "L5");
  }
});

test("input-only GPIOs have no output, PWM or internal pull capabilities", () => {
  const pins = defaults.available_pins.find(entry => entry.soc === "esp32").pins;
  for (const gpio of [34, 35, 36, 37, 38, 39]) {
    const pin = pins.find(entry => entry.gpio === gpio);
    assert.equal(pin.input, true);
    for (const capability of ["output", "pwm", "pull_up", "pull_down"]) {
      assert.ok(!pin[capability]);
    }
  }
});

test("catalogue defaults are embedded and imported without active assignments", () => {
  assert.equal(hardware["x-firmware-defaults"], undefined);
  assert.equal(defaults.configuration, undefined);
  assert.equal(defaults.pwm, undefined);
  const application = readFileSync(resolve(root, "app/application.cpp"), "utf8");
  assert.ok(application.includes('IMPORT_FSTR_LOCAL(hardware_defaults, PROJECT_DIR "/hardware-defaults.json")'));
  assert.ok(application.includes("FSTR::Stream defaultsStream(hardware_defaults)"));
  assert.ok(application.includes("hardwareUpdate.importFromStream(ConfigDB::Json::format, defaultsStream)"));
});

test("startup imports only a valid newer revision and retains the catalogue", () => {
  const application = readFileSync(resolve(root, "app/application.cpp"), "utf8");
  assert.ok(!application.includes("hardwareUpdate.availablePins.loadArrayDefaults()"));
  assert.ok(!application.includes("missingCatalogue"));
  const condition = application.match(/if\((defaultsVersion != uint32_t\(-1\)[\s\S]*?)\) \{/);
  assert.ok(condition, "Missing catalogue revision guard");
  const directory = mkdtempSync(join(tmpdir(), "lightinator-catalogue-revision-"));
  try {
    const executable = join(directory, "test");
    const source = `
#include <cassert>
#include <cstdint>
bool shouldImport(uint32_t defaultsVersion, uint32_t currentVersion)
{
    return ${condition[1]};
}
int main()
{
    assert(shouldImport(1, 0));
    assert(shouldImport(3, 2));
    assert(!shouldImport(2, 2));
    assert(!shouldImport(1, 2));
    assert(!shouldImport(uint32_t(-1), 0));
}
`;
    const compiler = spawnSync(process.env.CXX || "c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-x", "c++", "-", "-o", executable,
    ], { input: source, encoding: "utf8" });
    assert.equal(compiler.status, 0, compiler.stderr || String(compiler.error));
    const result = spawnSync(executable, [], { encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr || String(result.error));
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});