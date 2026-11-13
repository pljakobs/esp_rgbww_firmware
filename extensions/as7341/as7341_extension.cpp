#include "as7341_extension.h"

namespace
{
constexpr uint8_t address = 0x39;
}

As7341Extension::As7341Extension() : config("extension-as7341"), measurement(*this)
{
}

bool As7341Extension::read(uint8_t reg, uint8_t* data, size_t length)
{
	if(length > TwoWire::BUFFER_LENGTH) return false;
	Wire.beginTransmission(address);
	if(Wire.write(reg) != 1 || Wire.endTransmission(false) != TwoWire::I2C_ERR_SUCCESS) return false;
	if(Wire.requestFrom(address, uint8_t(length)) != length) return false;
	for(size_t i = 0; i < length; ++i) {
		int value = Wire.read();
		if(value < 0) return false;
		data[i] = uint8_t(value);
	}
	return true;
}

bool As7341Extension::write(uint8_t reg, const uint8_t* data, size_t length)
{
	if(length + 1 > TwoWire::BUFFER_LENGTH) return false;
	Wire.beginTransmission(address);
	if(Wire.write(reg) != 1 || Wire.write(data, length) != length) return false;
	return Wire.endTransmission() == TwoWire::I2C_ERR_SUCCESS;
}

void As7341Extension::configureBus(bool enabled)
{
	measurement.reset();
	ExtensionAs7341::Root settings(config);
	present = enabled && settings.getEnabled() && measurement.detect();
	if(enabled && settings.getEnabled()) {
		debug_i("AS7341: %s", present ? "detected at 0x39" : "not detected at 0x39");
	}
}

bool As7341Extension::start(std::function<void()> onCompletion)
{
	if(!present || busy()) return false;
	ExtensionAs7341::Root settings(config);
	As7341Measurement::Settings requested{settings.getAtime(), settings.getAstep(), settings.getGain()};
	completion = std::move(onCompletion);
	if(!measurement.start(requested, millis())) {
		completion = nullptr;
		return false;
	}
	pending = true;
	timer.initializeMs(10, TimerDelegate(&As7341Extension::poll, this)).start();
	return true;
}

void As7341Extension::poll()
{
	measurement.poll(millis());
	if(measurement.result().state != As7341Measurement::State::measuring) {
		timer.stop();
		pending = false;
		auto callback = std::move(completion);
		completion = nullptr;
		if(callback) callback();
	}
}
