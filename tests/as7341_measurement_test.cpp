#include "../extensions/as7341/measurement.h"
#include <cassert>
#include <cstring>

class FakeBus : public As7341Bus
{
public:
	uint8_t registers[256]{};
	bool failRead = false;
	bool failWrite = false;
	int failRegister = -1;
	bool read(uint8_t reg, uint8_t* data, size_t length) override
	{
		if(failRead || reg == failRegister) return false;
		std::memcpy(data, registers + reg, length);
		return true;
	}
	bool write(uint8_t reg, const uint8_t* data, size_t length) override
	{
		if(failWrite) return false;
		std::memcpy(registers + reg, data, length);
		return true;
	}
	void ready(unsigned base)
	{
		registers[0xa3] = 0x40;
		for(unsigned i = 0; i < 6; ++i) {
			registers[0x95 + i * 2] = uint8_t(base + i);
			registers[0x96 + i * 2] = 1;
		}
	}
	void finishSmux()
	{
		registers[0x80] &= ~0x10;
		registers[0xa3] = 0;
	}
};

int main()
{
	FakeBus bus;
	As7341Measurement sensor(bus);
	assert(!sensor.detect());
	bus.registers[0x92] = 0x27;
	assert(sensor.detect());
	bus.registers[0x70] = 3;
	assert(sensor.start({}, 100));
	assert((bus.registers[0x70] & 3) == 0 && (bus.registers[0xa9] & 0x10) == 0);
	assert(bus.registers[0x81] == 29);
	assert(bus.registers[0xca] == 87 && bus.registers[0xcb] == 2);
	assert(bus.registers[0xaa] == 6);
	assert(bus.registers[0] == 0x30 && bus.registers[5] == 0x42);
	assert(sensor.result().state == As7341Measurement::State::measuring);
	assert(!sensor.start({}, 101));
	sensor.poll(110);
	assert(sensor.result().state == As7341Measurement::State::measuring);
	bus.finishSmux();
	sensor.poll(120);
	bus.ready(10);
	sensor.poll(130);
	assert(sensor.result().state == As7341Measurement::State::measuring);
	sensor.poll(171);
	assert(bus.registers[3] == 0x40 && bus.registers[6] == 0x10);
	bus.finishSmux();
	sensor.poll(180);
	bus.ready(20);
	sensor.poll(231);
	assert(sensor.result().state == As7341Measurement::State::complete);
	assert(sensor.result().channels[0] == 266);
	assert(sensor.result().channels[5] == 271);
	assert(sensor.result().channels[6] == 276);
	assert(sensor.result().channels[11] == 281);
	assert(sensor.result().completedAt == 231);
	assert(sensor.start({}, 300));
	assert(sensor.result().state == As7341Measurement::State::measuring);
	sensor.poll(1301);
	assert(sensor.result().state == As7341Measurement::State::error);
	assert(std::strcmp(sensor.result().error, "SMUX timeout") == 0);
	bus.failWrite = true;
	assert(sensor.start({}, 1400));
	assert(sensor.result().state == As7341Measurement::State::error);
	bus.failWrite = false;
	assert(sensor.start({}, 1500));
	bus.failRead = true;
	sensor.poll(1510);
	assert(sensor.result().state == As7341Measurement::State::error);
	assert(std::strcmp(sensor.result().error, "I2C acquisition and cleanup failed") == 0);
	bus.failRead = false;
	assert(sensor.start({}, 2000));
	bus.finishSmux();
	sensor.poll(2010);
	bus.ready(10);
	bus.failRegister = 0x95;
	sensor.poll(2061);
	assert(sensor.result().state == As7341Measurement::State::error);
	assert(std::strcmp(sensor.result().error, "I2C channel read failed") == 0);
	assert(sensor.result().channels[0] == 266);
	bus.failRegister = -1;
	assert(sensor.start({}, 3000));
	bus.finishSmux();
	sensor.poll(3010);
	sensor.poll(4113);
	assert(sensor.result().state == As7341Measurement::State::error);
	assert(std::strcmp(sensor.result().error, "Integration timeout") == 0);
	sensor.reset();
	assert(sensor.result().state == As7341Measurement::State::idle);
}
