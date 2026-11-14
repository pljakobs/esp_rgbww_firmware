#pragma once

#include "measurement.h"
#include <SmingCore.h>
#include <Wire.h>
#include <extension_ina219.h>
#include <functional>

class Ina219Extension : private Ina219Bus
{
public:
	struct Settings {
		bool enabled;
		uint8_t address;
		double shuntOhms;
		uint32_t intervalMs;
	};
	Ina219Extension();
	void configureBus(bool enabled);
	bool available() const { return present; }
	Settings settings();
	bool setSettings(const Settings& settings);
	void setUpdateHandler(std::function<void()> handler) { onUpdate = std::move(handler); }
	const Ina219Measurement::Result& result() const { return measurement.result(); }
	static constexpr const char* id = "current-1";

private:
	bool read(uint8_t reg, uint16_t& value) override;
	bool write(uint8_t reg, uint16_t value) override;
	void sample();
	ExtensionIna219 config;
	Ina219Measurement measurement;
	Timer timer;
	uint8_t address = 0x40;
	bool busEnabled = false;
	bool present = false;
	std::function<void()> onUpdate;
};
