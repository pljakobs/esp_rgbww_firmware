#pragma once

#include <stdint.h>
#include <stddef.h>

class As7341Bus
{
public:
	virtual ~As7341Bus() = default;
	virtual bool read(uint8_t reg, uint8_t* data, size_t length) = 0;
	virtual bool write(uint8_t reg, const uint8_t* data, size_t length) = 0;
};

class As7341Measurement
{
public:
	enum class State { idle, measuring, complete, error };
	struct Settings {
		uint8_t atime = 29;
		uint16_t astep = 599;
		uint8_t gain = 6;
	};
	struct Result {
		State state = State::idle;
		const char* error = "";
		uint16_t channels[12]{};
		Settings settings{};
		uint32_t completedAt = 0;
	};

	explicit As7341Measurement(As7341Bus& bus) : bus(bus) {}
	bool detect();
	bool start(Settings settings, uint32_t now);
	void poll(uint32_t now);
	void reset() { current = {}; }
	const Result& result() const { return current; }

private:
	enum class Phase { smuxLow, low, smuxHigh, high };
	bool writeByte(uint8_t reg, uint8_t value);
	bool updateBits(uint8_t reg, uint8_t mask, uint8_t value);
	bool configureSmux(bool high, uint32_t now);
	void fail(const char* message);
	As7341Bus& bus;
	Result current{};
	Phase phase = Phase::smuxLow;
	uint32_t phaseStarted = 0;
	uint32_t integrationMs = 0;
	uint32_t integrationTimeout = 0;
	uint16_t samples[12]{};
};
