#include "measurement.h"

namespace
{
constexpr uint8_t ENABLE = 0x80;
constexpr uint8_t CONFIG = 0x70;
constexpr uint8_t ATIME = 0x81;
constexpr uint8_t ID = 0x92;
constexpr uint8_t DATA = 0x95;
constexpr uint8_t STATUS2 = 0xa3;
constexpr uint8_t CFG0 = 0xa9;
constexpr uint8_t CFG1 = 0xaa;
constexpr uint8_t CFG6 = 0xaf;
constexpr uint8_t ASTEP = 0xca;
constexpr uint32_t SMUX_TIMEOUT_MS = 1000;

// SMUX mappings from the Adafruit AS7341 driver (BSD license).
constexpr uint8_t lowMapping[] = {
	0x30, 0x01, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x50, 0x00,
	0x00, 0x00, 0x20, 0x04, 0x00, 0x30, 0x01, 0x50, 0x00, 0x06,
};
constexpr uint8_t highMapping[] = {
	0x00, 0x00, 0x00, 0x40, 0x02, 0x00, 0x10, 0x03, 0x50, 0x10,
	0x03, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x50, 0x00, 0x06,
};
}

bool As7341Measurement::writeByte(uint8_t reg, uint8_t value)
{
	return bus.write(reg, &value, 1);
}

bool As7341Measurement::updateBits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t old;
	return bus.read(reg, &old, 1) && writeByte(reg, (old & ~mask) | (value & mask));
}

bool As7341Measurement::detect()
{
	uint8_t id;
	return bus.read(ID, &id, 1) && (id & 0xfc) == 0x24;
}

bool As7341Measurement::start(Settings settings, uint32_t now)
{
	if(current.state == State::measuring || settings.gain > 10) {
		return false;
	}
	current.state = State::measuring;
	current.error = "";
	current.settings = settings;
	const uint64_t microseconds = uint64_t(settings.atime + 1) * (uint32_t(settings.astep) + 1) * 278 / 100;
	integrationMs = uint32_t((microseconds + 999) / 1000);
	integrationTimeout = integrationMs * 2 + 1000;
	const uint8_t astep[] = {uint8_t(settings.astep), uint8_t(settings.astep >> 8)};
	if(!writeByte(ENABLE, 1)) {
		fail("I2C setup failed");
		return true;
	}
	const bool modeConfigured = updateBits(CFG0, 0x10, 0x10) && updateBits(CONFIG, 0x03, 0);
	const bool bankRestored = updateBits(CFG0, 0x10, 0);
	if(!modeConfigured || !bankRestored || !writeByte(ATIME, settings.atime) ||
	   !bus.write(ASTEP, astep, sizeof(astep)) || !writeByte(CFG1, settings.gain) ||
	   !configureSmux(false, now)) {
		fail("I2C setup failed");
	}
	return true;
}

bool As7341Measurement::configureSmux(bool high, uint32_t now)
{
	const uint8_t* mapping = high ? highMapping : lowMapping;
	if(!updateBits(ENABLE, 0x02, 0) || !updateBits(CFG6, 0x18, 0x10) ||
	   !bus.write(0, mapping, sizeof(lowMapping)) || !updateBits(ENABLE, 0x10, 0x10)) {
		return false;
	}
	phase = high ? Phase::smuxHigh : Phase::smuxLow;
	phaseStarted = now;
	return true;
}

void As7341Measurement::fail(const char* message)
{
	current.state = State::error;
	current.error = updateBits(ENABLE, 0x02, 0) ? message : "I2C acquisition and cleanup failed";
}

void As7341Measurement::poll(uint32_t now)
{
	if(current.state != State::measuring) {
		return;
	}
	const bool smux = phase == Phase::smuxLow || phase == Phase::smuxHigh;
	const uint32_t timeout = smux ? SMUX_TIMEOUT_MS : integrationTimeout;
	if(uint32_t(now - phaseStarted) > timeout) {
		fail(smux ? "SMUX timeout" : "Integration timeout");
		return;
	}
	uint8_t status;
	if(!bus.read(smux ? ENABLE : STATUS2, &status, 1)) {
		fail("I2C status read failed");
		return;
	}
	if(smux) {
		if(status & 0x10) {
			return;
		}
		if(!updateBits(ENABLE, 0x02, 0x02)) {
			fail("I2C measurement start failed");
			return;
		}
		phase = phase == Phase::smuxLow ? Phase::low : Phase::high;
		phaseStarted = now;
		return;
	}
	if(uint32_t(now - phaseStarted) < integrationMs) {
		return;
	}
	if(!(status & 0x40)) {
		return;
	}
	uint8_t bytes[12];
	if(!bus.read(DATA, bytes, sizeof(bytes))) {
		fail("I2C channel read failed");
		return;
	}
	const unsigned offset = phase == Phase::low ? 0 : 6;
	for(unsigned i = 0; i < 6; ++i) {
		samples[offset + i] = uint16_t(bytes[2 * i]) | (uint16_t(bytes[2 * i + 1]) << 8);
	}
	if(phase == Phase::low) {
		if(!configureSmux(true, now)) {
			fail("I2C SMUX setup failed");
		}
		return;
	}
	if(!updateBits(ENABLE, 0x02, 0)) {
		fail("I2C measurement stop failed");
		return;
	}
	for(unsigned i = 0; i < 12; ++i) {
		current.channels[i] = samples[i];
	}
	current.completedAt = now;
	current.state = State::complete;
}
