#include "measurement.h"
#include <cmath>

bool Ina219Measurement::detect()
{
	uint16_t value;
	// INA219 has no device ID. Reserved/reset bits must read as zero.
	if(!bus.read(0, value) || (value & 0xc000)) return false;
	constexpr uint16_t config = 0x399f; // 32 V, +/-320 mV, 12-bit continuous
	return bus.write(0, config) && bus.read(0, value) && value == config;
}

bool Ina219Measurement::fail(const char* error)
{
	current.valid = false;
	current.error = error;
	return false;
}

bool Ina219Measurement::sample(float shuntOhms, uint32_t now)
{
	if(!std::isfinite(shuntOhms) || shuntOhms < 0.00001f || shuntOhms > 1.0f) {
		return fail("invalid shunt resistance");
	}
	uint16_t busVoltage, shuntVoltage;
	if(!bus.read(2, busVoltage) || !bus.read(1, shuntVoltage)) return fail("I2C read failed");
	if(!(busVoltage & 2)) return fail("conversion not ready");
	const int32_t signedShunt = shuntVoltage & 0x8000 ? int32_t(shuntVoltage) - 65536 : shuntVoltage;
	if(signedShunt >= 32000 || signedShunt <= -32000 || (busVoltage >> 3) >= 8000) {
		return fail("measurement out of range");
	}
	current.voltage = (busVoltage >> 3) * 0.004f;
	current.current = signedShunt * 0.00001f / shuntOhms;
	current.power = current.voltage * current.current;
	current.completedAt = now;
	current.error = "";
	current.valid = true;
	return true;
}
