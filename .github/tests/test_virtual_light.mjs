import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");

test("VirtualLight exclusively owns a library instance across vector moves", () => {
  const directory = mkdtempSync(join(tmpdir(), "lightinator-virtual-light-"));
  try {
    mkdirSync(join(directory, "RGBWWLed"));
    writeFileSync(join(directory, "WString.h"), "#pragma once\n#include <string>\nusing String = std::string;\n");
    writeFileSync(join(directory, "RGBWWLed/RGBWWLed.h"), `
#pragma once
class RGBWWLed {
public:
    inline static int instances = 0;
    RGBWWLed() { ++instances; }
    ~RGBWWLed() { --instances; }
};
`);
    writeFileSync(join(directory, "test.cpp"), `
#include <virtuallight.h>
#include <cassert>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(!std::is_copy_constructible_v<VirtualLight>);
static_assert(std::is_move_constructible_v<VirtualLight>);
static_assert(std::is_same_v<decltype(VirtualLight::led), std::unique_ptr<RGBWWLed>>);

int main()
{
    {
        std::vector<VirtualLight> lights;
        lights.push_back({"light0", "Main", std::make_unique<RGBWWLed>()});
        RGBWWLed* original = lights.front().led.get();
        for (int index = 1; index < 16; ++index) {
            lights.push_back({"light" + std::to_string(index), "Other", std::make_unique<RGBWWLed>()});
        }
        assert(lights.front().id == "light0");
        assert(lights.front().name == "Main");
        assert(lights.front().led.get() == original);
        assert(RGBWWLed::instances == 16);
        auto moved = std::move(lights);
        assert(moved.front().led.get() == original);
        moved.erase(moved.begin() + 1);
        assert(moved[1].id == "light2");
        assert(RGBWWLed::instances == 15);
        moved.clear();
        assert(RGBWWLed::instances == 0);
    }
    assert(RGBWWLed::instances == 0);
}
`);
    const executable = join(directory, "test");
    const compiler = spawnSync(process.env.CXX || "c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror",
      `-I${directory}`, `-I${join(root, "include")}`,
      join(directory, "test.cpp"), "-o", executable,
    ], { encoding: "utf8" });
    assert.equal(compiler.status, 0, compiler.stderr || String(compiler.error));
    const result = spawnSync(executable, [], { encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr || String(result.error));
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("the firmware owns the virtual-light vector", () => {
  const header = readFileSync(join(root, "include/application.h"), "utf8");
  assert.match(header, /std::vector<VirtualLight> lights;/);
});