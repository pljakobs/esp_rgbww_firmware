#include "ina219_extension.h"
#include <cmath>

Ina219Extension::Ina219Extension() : config("extension-ina219"), measurement(*this) {}

Ina219Extension::Settings Ina219Extension::settings()
{
	ExtensionIna219::Root root(config);
	return {root.getEnabled(), root.getAddress(), root.getShuntOhms().asFloat(), root.getIntervalMs()};
}

bool Ina219Extension::read(uint8_t reg, uint16_t& value)
{
	Wire.beginTransmission(address);
	if(Wire.write(reg) != 1 || Wire.endTransmission(false) != TwoWire::I2C_ERR_SUCCESS) return false;
	if(Wire.requestFrom(address, uint8_t(2)) != 2) return false;
	int high = Wire.read(), low = Wire.read();
	if(high < 0 || low < 0) return false;
	value = (uint16_t(high) << 8) | uint16_t(low);
	return true;
}

bool Ina219Extension::write(uint8_t reg, uint16_t value)
{
	Wire.beginTransmission(address);
	if(Wire.write(reg) != 1 || Wire.write(uint8_t(value >> 8)) != 1 || Wire.write(uint8_t(value)) != 1) return false;
	return Wire.endTransmission() == TwoWire::I2C_ERR_SUCCESS;
}

void Ina219Extension::configureBus(bool enabled)
{
	timer.stop();
	measurement.reset();
	busEnabled = enabled;
	auto current = settings();
	address = current.address;
	present = enabled && current.enabled && measurement.detect();
	measurement.invalidate(present ? "waiting for measurement" : "extension unavailable");
	if(enabled && current.enabled) {
		debug_i("INA219: %s at 0x%02x", present ? "detected" : "not detected", address);
	}
	if(present) timer.initializeMs(current.intervalMs, TimerDelegate(&Ina219Extension::sample, this)).start();
	if(onUpdate) onUpdate();
}

void Ina219Extension::sample()
{
	if(!measurement.sample(settings().shuntOhms, millis())) {
		debug_w("INA219: %s", measurement.result().error);
	}
	if(onUpdate) onUpdate();
}

bool Ina219Extension::setSettings(const Settings& next)
{
	if(next.address < 0x40 || next.address > 0x4f || !std::isfinite(next.shuntOhms) ||
	   next.shuntOhms < 0.00001 || next.shuntOhms > 1 || next.intervalMs < 200 || next.intervalMs > 3600000) {
		return false;
	}
	const auto previous = settings();
	{
		ExtensionIna219::Root root(config);
		auto update = root.update();
		if(!update) return false;
		update.setEnabled(next.enabled);
		update.setAddress(next.address);
		update.setShuntOhms(ConfigDB::Number(next.shuntOhms));
		update.setIntervalMs(next.intervalMs);
		if(!update.commit()) {
			update.setEnabled(previous.enabled);
			update.setAddress(previous.address);
			update.setShuntOhms(ConfigDB::Number(previous.shuntOhms));
			update.setIntervalMs(previous.intervalMs);
			update.clearDirty();
			return false;
		}
	}
	configureBus(busEnabled);
	return true;
}
