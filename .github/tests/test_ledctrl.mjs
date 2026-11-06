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
  struct Pin {
    String id;
    int gpio = 0;
    std::vector<String> allowedFunctions{"light.pwm"};
    std::vector<String> restrictions;
    String getId() const { return id; }
    int getGpio() const { return gpio; }
  };
  struct Capability {
    int gpio = 0;
    uint8_t capability = CAPA_INPUT | CAPA_OUTPUT | CAPA_PWM | CAPA_PULL_UP | CAPA_PULL_DOWN;
    int getGpio() const { return gpio; }
    uint8_t getCapability() const { return capability; }
  };
  struct Available {
    Items<Capability> pins;
    String getSoc() const { return "esp32"; }
  };
  struct Board {
    Items<Pin> implementedPins;
    std::vector<int> reservedGpios;
    String getId() const { return "board"; }
    unsigned getRevision() const { return 1; }
    String getSoc() const { return "esp32"; }
  };
  struct Light {
    String id;
    String type;
    Items<String> pins;
    String getId() const { return id; }
    String getName() const { return id; }
    String getType() const { return type; }
  };
  struct System {
    String pin;
    String getClearConfigPin() const { return pin; }
  };
  struct Button { String getPin() const { return "L1"; } };
  struct Bus {
    String getSda() const { return "SDA"; }
    String getScl() const { return "SCL"; }
  };
  struct Configuration {
    Items<Light> lights;
    Items<Button> buttons;
    Items<Bus> buses;
    System system;
    String getBoardId() const { return "board"; }
    unsigned getBoardRevision() const { return 1; }
  };
  struct Hardware {
    Configuration configuration;
    Items<Board> boards;
    Items<Available> availablePins;
  };
  ${text.slice(begin, end)}
  int main()
  {
    Hardware hardware;
    Board board;
    Available available;
    for(int index = 0; index < 5; ++index) {
      board.implementedPins.push_back({"L" + std::to_string(index + 1), index, {"light.pwm"}, {}});
      available.pins.push_back({index});
    }
    hardware.boards.push_back(board);
    hardware.availablePins.push_back(available);
    hardware.configuration.lights.push_back({"rgb0", "RGB", {"L1", "L2", "L3"}});
    hardware.configuration.lights.push_back({"white0", "WWCW", {"L4", "L5"}});
    std::vector<ResolvedLight> resolved;
    String error;
    assert(resolveConfiguredLights(hardware, "esp32", 5, resolved, error));
    assert(resolved.size() == 2 && resolved[0].type == LightType::RGB && resolved[0].pins[0] == 0);
    assert(resolved[1].type == LightType::WWCW && resolved[1].pins[3] == 3 && resolved[1].pins[4] == 4);
    assert(!resolveConfiguredLights(hardware, "esp32", 4, resolved, error));
    auto invalid = hardware;
    invalid.configuration.lights[1].pins[0] = "L1";
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.configuration.lights[1].id = "rgb0";
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.availablePins[0].pins[0].capability &= ~CAPA_PWM;
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.availablePins[0].pins[0].capability |= CAPA_RESERVED;
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.boards[0].reservedGpios.push_back(0);
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.configuration.lights[0].pins[0] = "unknown";
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.boards[0].implementedPins[0].allowedFunctions.push_back("system.clear_config");
    invalid.configuration.system.pin = "L1";
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    assert(!resolveConfiguredLights(hardware, "esp32c3", 5, resolved, error));
    hardware.configuration.lights.clear();
    assert(resolveConfiguredLights(hardware, "esp32", 5, resolved, error) && resolved.empty());
  }
  `);
  });