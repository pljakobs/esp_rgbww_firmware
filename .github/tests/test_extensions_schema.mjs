import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const config = JSON.parse(readFileSync(new URL("../../app-config.cfgdb", import.meta.url)));
const extensions = config.properties.extensions;

test("extension settings have a dedicated store and independent device settings", () => {
  assert.equal(extensions.store, true);
  assert.deepEqual(Object.keys(extensions.properties).sort(), [
    "color_sensor", "current_sensor", "display",
  ]);
  for (const device of Object.values(extensions.properties)) {
    assert.equal(device.properties.enabled.default, true);
  }
});

test("INA219 defaults to the standard shunt and a valid I2C address", () => {
  const sensor = extensions.properties.current_sensor.properties;
  assert.equal(sensor.shunt_ohms.default, 0.1);
  assert.ok(sensor.shunt_ohms.minimum > 0);
  assert.equal(sensor.address.default, 0x40);
  assert.equal(sensor.address.minimum, 0x40);
  assert.equal(sensor.address.maximum, 0x4f);
});

test("AS7341 integration and gain settings fit their registers", () => {
  const sensor = extensions.properties.color_sensor.properties;
  assert.equal(sensor.atime.maximum, 255);
  assert.equal(sensor.astep.maximum, 65535);
  assert.equal(sensor.gain.maximum, 10);
  for (const setting of [sensor.atime, sensor.astep, sensor.gain, sensor.interval_ms]) {
    assert.ok(setting.default >= setting.minimum && setting.default <= setting.maximum);
  }
});
