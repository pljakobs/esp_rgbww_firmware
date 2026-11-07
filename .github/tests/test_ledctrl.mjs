import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");

function compileAndRun(source) {
  const directory = mkdtempSync(join(tmpdir(), "lightinator-ledctrl-"));
  try {
    const executable = join(directory, "test");
    const compiler = spawnSync(process.env.CXX || "c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-x", "c++", "-", "-o", executable,
    ], { input: source, encoding: "utf8" });
    assert.equal(compiler.status, 0, compiler.stderr || String(compiler.error));
    const result = spawnSync(executable, [], { encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr || String(result.error));
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
}

test("configured light types require the library's exact pin count", () => {
  const text = readFileSync(join(root, "app/ledctrl.cpp"), "utf8");
  const begin = text.indexOf("bool selectLightType(");
  const end = text.indexOf("\n}\n", begin) + 2;
  assert.ok(begin >= 0 && end > begin);
  const source = `
#include <array>
#include <cassert>
#include <string>
using String = std::string;
#define F(value) value
enum class LightType { SINGLE, WWCW, WW, CW, RGB, RGBWW, RGBCW, RGBWWCW };
${text.slice(begin, end)}
int main()
{
    LightType mode{};
  assert(selectLightType("SINGLE", 1, mode));
    assert(mode == LightType::SINGLE);
  assert(selectLightType("WWCW", 2, mode));
    assert(mode == LightType::WWCW);
  assert(selectLightType("WW", 1, mode) && mode == LightType::WW);
  assert(selectLightType("CW", 1, mode) && mode == LightType::CW);
  assert(selectLightType("RGB", 3, mode));
    assert(mode == LightType::RGB);
  assert(selectLightType("RGBWW", 4, mode));
    assert(mode == LightType::RGBWW);
  assert(selectLightType("RGBCW", 4, mode));
    assert(mode == LightType::RGBCW);
  assert(selectLightType("RGBWWCW", 5, mode));
    assert(mode == LightType::RGBWWCW);
  assert(!selectLightType("RGB", 2, mode));
  assert(!selectLightType("RGBCW", 5, mode));
  assert(!selectLightType("SINGLE", 2, mode));
  assert(!selectLightType("unknown", 0, mode));
}
`;
    compileAndRun(source);
});

  test("configured lights resolve GPIO0 and reject cross-light resource conflicts", () => {
    const text = readFileSync(join(root, "app/ledctrl.cpp"), "utf8");
    const begin = text.indexOf("bool selectLightType(");
    const end = text.indexOf("template <typename Color>", begin);
    assert.ok(begin >= 0 && end > begin);
    compileAndRun(`
  #include <algorithm>
  #include <array>
  #include <cassert>
#include <cstdint>
  #include <string>
  #include <utility>
  #include <vector>
  using String = std::string;
  #define F(value) value
  enum PinCapability : uint8_t {
    CAPA_INPUT = 1u << 0, CAPA_OUTPUT = 1u << 1, CAPA_PWM = 1u << 2,
    CAPA_STRAPPING = 1u << 3, CAPA_PULL_UP = 1u << 4, CAPA_PULL_DOWN = 1u << 5, CAPA_RESERVED = 1u << 6
  };
  enum class LightType { SINGLE, WWCW, WW, CW, RGB, RGBWW, RGBCW, RGBWWCW };
  template<class Item> struct Items : std::vector<Item> {
    using std::vector<Item>::vector;
    unsigned getItemCount() const { return this->size(); }
  };
  struct Capability {
    int gpio = 0;
    uint8_t capability = CAPA_INPUT | CAPA_OUTPUT | CAPA_PWM | CAPA_PULL_UP | CAPA_PULL_DOWN;
    int getGpio() const { return gpio; }
    uint8_t getCapability() const { return capability; }
  };
  struct Available {
    Items<Capability> pins;
    String soc = "esp32";
    String getSoc() const { return soc; }
  };
  struct Light {
    String id;
    String type;
    Items<int> pins;
    uint16_t index = 0;
    bool isDefault = false;
    String getId() const { return id; }
    String getName() const { return id; }
    String getType() const { return type; }
    uint16_t getIndex() const { return index; }
    bool getIsDefault() const { return isDefault; }
  };
  struct System { bool getClearConfigEnabled() const { return false; } int getClearConfigPin() const { return 0; } };
  struct Button { int getPin() const { return 0; } };
  struct Bus {
    int getSda() const { return 0; }
    int getScl() const { return 1; }
  };
  struct Configuration {
    Items<Light> lights;
    Items<Button> buttons;
    Items<Bus> buses;
    System system;
    String soc = "esp32";
    String getSoc() const { return soc; }
  };
  struct Hardware {
    Configuration configuration;
  };
  struct PinDatabase {
    Items<Available> availablePins;
  };
  ${text.slice(begin, end)}
  int main()
  {
    Hardware hardware;
    PinDatabase pinDatabase;
    Available available;
    for(int index = 0; index < 5; ++index) {
      available.pins.push_back({index});
    }
    pinDatabase.availablePins.push_back(available);
    hardware.configuration.lights.push_back({"rgb0", "RGB", {0, 1, 2}, 10, true});
    hardware.configuration.lights.push_back({"white0", "WWCW", {3, 4}, 23, false});
    std::vector<ResolvedLight> resolved;
    String error;
    assert(resolveConfiguredLights(hardware, pinDatabase, "esp32", 5, resolved, error));
    assert(resolved.size() == 2 && resolved[0].type == LightType::RGB && resolved[0].pins[0] == 0);
    assert(resolved[1].type == LightType::WWCW && resolved[1].pins[3] == 3 && resolved[1].pins[4] == 4);
    assert(resolved[0].index == 10 && resolved[0].isDefault);
    assert(!resolveConfiguredLights(hardware, pinDatabase, "esp32", 4, resolved, error));
    auto invalid = hardware;
    invalid.configuration.lights[1].pins[0] = 0;
    assert(!resolveConfiguredLights(invalid, pinDatabase, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.configuration.lights[1].id = "rgb0";
    assert(!resolveConfiguredLights(invalid, pinDatabase, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.configuration.lights[0].isDefault = false;
    assert(!resolveConfiguredLights(invalid, pinDatabase, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.configuration.lights[1].isDefault = true;
    assert(!resolveConfiguredLights(invalid, pinDatabase, "esp32", 5, resolved, error));
    auto invalidPins = pinDatabase;
    invalidPins.availablePins[0].pins[0].capability &= ~CAPA_PWM;
    assert(!resolveConfiguredLights(hardware, invalidPins, "esp32", 5, resolved, error));
    invalidPins = pinDatabase;
    invalidPins.availablePins[0].pins[0].capability |= CAPA_RESERVED;
    assert(!resolveConfiguredLights(hardware, invalidPins, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.configuration.lights[0].pins[0] = 99;
    assert(!resolveConfiguredLights(invalid, pinDatabase, "esp32", 5, resolved, error));
    assert(!resolveConfiguredLights(hardware, pinDatabase, "esp32c3", 5, resolved, error));
    hardware.configuration.lights.clear();
    assert(resolveConfiguredLights(hardware, pinDatabase, "esp32", 5, resolved, error) && resolved.empty());
  }
  `);
  });