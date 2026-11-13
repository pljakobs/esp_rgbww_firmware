#pragma once

#include "measurement.h"
#include <SmingCore.h>
#include <Wire.h>
#include <extension_as7341.h>
#include <functional>

class As7341Extension : private As7341Bus
{
public:
	As7341Extension();
	void configureBus(bool enabled);
	bool available() const { return present; }
	bool busy() const { return pending; }
	bool start(std::function<void()> completion);
	const As7341Measurement::Result& result() const { return measurement.result(); }
	static constexpr const char* id = "spectrum-1";

private:
	bool read(uint8_t reg, uint8_t* data, size_t length) override;
	bool write(uint8_t reg, const uint8_t* data, size_t length) override;
	void poll();
	ExtensionAs7341 config;
	As7341Measurement measurement;
	Timer timer;
	std::function<void()> completion;
	bool present = false;
	bool pending = false;
};
