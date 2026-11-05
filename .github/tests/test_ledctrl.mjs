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

test("configured light roles select library modes without silent fallback", () => {
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
enum class LightType { SINGLE, WWCW, RGB, RGBWW, RGBCW, RGBWWCW };
${text.slice(begin, end)}
int main()
{
    LightType mode{};
    assert(selectLightType("dimmer", {"", "", "", "", "", "L1"}, mode));
    assert(mode == LightType::SINGLE);
    assert(selectLightType("tunable_white", {"", "", "", "L1", "L2", ""}, mode));
    assert(mode == LightType::WWCW);
    assert(selectLightType("rgb", {"L1", "L2", "L3", "", "", ""}, mode));
    assert(mode == LightType::RGB);
    assert(selectLightType("rgbw", {"L1", "L2", "L3", "L4", "", ""}, mode));
    assert(mode == LightType::RGBWW);
    assert(selectLightType("rgbw", {"L1", "L2", "L3", "", "L4", ""}, mode));
    assert(mode == LightType::RGBCW);
    assert(selectLightType("rgbww", {"L1", "L2", "L3", "L4", "L5", ""}, mode));
    assert(mode == LightType::RGBWWCW);
    assert(!selectLightType("rgb", {"L1", "", "L3", "", "", ""}, mode));
    assert(!selectLightType("rgbw", {"L1", "L2", "L3", "L4", "L5", ""}, mode));
    assert(!selectLightType("dimmer", {"L1", "", "", "", "", "L2"}, mode));
    assert(!selectLightType("unknown", {}, mode));
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
  #include <string>
  #include <utility>
  #include <vector>
  using String = std::string;
  #define F(value) value
  enum class LightType { SINGLE, WWCW, RGB, RGBWW, RGBCW, RGBWWCW };
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
    bool pwm = true;
    bool reserved = false;
    int getGpio() const { return gpio; }
    bool getOutput() const { return pwm; }
    bool getInput() const { return true; }
    bool getPwm() const { return pwm; }
    bool getReserved() const { return reserved; }
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
  struct Outputs {
    std::array<String,6> roles{};
    String getRed() const { return roles[0]; }
    String getGreen() const { return roles[1]; }
    String getBlue() const { return roles[2]; }
    String getWarmwhite() const { return roles[3]; }
    String getColdwhite() const { return roles[4]; }
    String getBrightness() const { return roles[5]; }
  };
  struct Light {
    String id;
    String type;
    Outputs outputs;
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
      available.pins.push_back({index, true, false});
    }
    hardware.boards.push_back(board);
    hardware.availablePins.push_back(available);
    hardware.configuration.lights.push_back({"rgb0", "rgb", {{{"L1", "L2", "L3", "", "", ""}}}});
    hardware.configuration.lights.push_back({"white0", "tunable_white", {{{"", "", "", "L4", "L5", ""}}}});
    std::vector<ResolvedLight> resolved;
    String error;
    assert(resolveConfiguredLights(hardware, "esp32", 5, resolved, error));
    assert(resolved.size() == 2 && resolved[0].pins[0] == 0 && resolved[1].pins[3] == 3);
    assert(!resolveConfiguredLights(hardware, "esp32", 4, resolved, error));
    auto invalid = hardware;
    invalid.configuration.lights[1].outputs.roles[3] = "L1";
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.configuration.lights[1].id = "rgb0";
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.availablePins[0].pins[0].pwm = false;
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.availablePins[0].pins[0].reserved = true;
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.boards[0].reservedGpios.push_back(0);
    assert(!resolveConfiguredLights(invalid, "esp32", 5, resolved, error));
    invalid = hardware;
    invalid.configuration.lights[0].outputs.roles[0] = "unknown";
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