#pragma once

#include <stdint.h>

class Ina219Bus
{
public:
	virtual ~Ina219Bus() = default;
	virtual bool read(uint8_t reg, uint16_t& value) = 0;
	virtual bool write(uint8_t reg, uint16_t value) = 0;
};

class Ina219Measurement
{
public:
	struct Result {
		bool valid = false;
		const char* error = "";
		float voltage = 0;
		float current = 0;
		float power = 0;
		uint32_t completedAt = 0;
	};

	explicit Ina219Measurement(Ina219Bus& bus) : bus(bus) {}
	bool detect();
	bool sample(float shuntOhms, uint32_t now);
	void reset() { current = {}; }
	void invalidate(const char* error) { current = {}; current.error = error; }
	const Result& result() const { return current; }

private:
	bool fail(const char* error);
	Ina219Bus& bus;
	Result current{};
};
