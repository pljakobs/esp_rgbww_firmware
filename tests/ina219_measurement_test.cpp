#include "../extensions/ina219/measurement.h"
#include <cassert>
#include <cmath>
#include <cstdint>

class FakeBus : public Ina219Bus
{
public:
	uint16_t regs[3]{0x399f, 0, 0};
	bool readOk = true;
	bool writeOk = true;
	bool read(uint8_t reg, uint16_t& value) override
	{
		if(!readOk) return false;
		value = regs[reg];
		return true;
	}
	bool write(uint8_t reg, uint16_t value) override
	{
		if(!writeOk) return false;
		regs[reg] = value;
		return true;
	}
};

int main()
{
	FakeBus bus;
	Ina219Measurement sensor(bus);
	assert(sensor.detect());
	bus.regs[2] = (3000u << 3) | 2;
	bus.regs[1] = 10000;
	assert(sensor.sample(0.1f, 123));
	auto result = sensor.result();
	assert(result.valid && result.completedAt == 123);
	assert(std::fabs(result.voltage - 12) < 0.001f);
	assert(std::fabs(result.current - 1) < 0.001f);
	assert(std::fabs(result.power - 12) < 0.001f);
	bus.regs[1] = uint16_t(65536 - 10000);
	assert(sensor.sample(0.2f, 200));
	assert(std::fabs(sensor.result().current + 0.5f) < 0.001f);
	assert(sensor.result().power < 0);
	bus.regs[2] &= ~2u;
	assert(!sensor.sample(0.1f, 300) && !sensor.result().valid);
	bus.regs[2] |= 2;
	bus.regs[1] = 32000;
	assert(!sensor.sample(0.1f, 300) && !sensor.result().valid);
	bus.regs[1] = 0;
	bus.readOk = false;
	assert(!sensor.sample(0.1f, 300) && !sensor.result().valid);
	assert(!sensor.detect());
	bus.readOk = true;
	bus.writeOk = false;
	assert(!sensor.detect());
	bus.writeOk = true;
	bus.regs[0] = 0x4000;
	assert(!sensor.detect());
	assert(!sensor.sample(0, 300));
	sensor.reset();
	assert(!sensor.result().valid && sensor.result().error[0] == '\0');
	sensor.invalidate("extension unavailable");
	assert(!sensor.result().valid && sensor.result().error[0] != '\0');
}
