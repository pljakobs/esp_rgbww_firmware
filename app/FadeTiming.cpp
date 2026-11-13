#include <FadeTiming.h>

#if FADE_TIMING_DIAGNOSTICS && defined(ARCH_ESP32)
#include <SmingCore.h>
#include <Data/Stream/MemoryDataStream.h>
#include <esp_timer.h>
#include <atomic>

namespace
{
struct Entry {
	int64_t us;
	uint32_t key;
	uint32_t value;
	uint32_t detail;
	FadeTiming::Event event;
};
constexpr unsigned Capacity = 1024;
Entry entries[Capacity];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> recording{false};
uint16_t lightIndex;
unsigned count;
unsigned dropped;
}

uint32_t IRAM_ATTR FadeTiming::record(Event event, uint32_t key, uint32_t value, uint32_t detail)
{
	if(!recording.load(std::memory_order_relaxed)) return 0;
	portENTER_CRITICAL_SAFE(&mux);
	uint32_t id = 0;
	if(recording.load(std::memory_order_relaxed)) {
		if(count < Capacity) {
			entries[count] = {esp_timer_get_time(), key, value, detail, event};
			id = ++count;
		} else {
			++dropped;
		}
	}
	portEXIT_CRITICAL_SAFE(&mux);
	return id;
}

bool FadeTiming::selected(uint16_t light)
{
	return recording.load(std::memory_order_relaxed) && light == lightIndex;
}

void FadeTiming::start(uint16_t light)
{
	portENTER_CRITICAL_SAFE(&mux);
	recording.store(false, std::memory_order_relaxed);
	count = dropped = 0;
	lightIndex = light;
	recording.store(true, std::memory_order_relaxed);
	portEXIT_CRITICAL_SAFE(&mux);
}

void FadeTiming::exportCsv(MemoryDataStream& stream)
{
	portENTER_CRITICAL_SAFE(&mux);
	recording.store(false, std::memory_order_relaxed);
	portEXIT_CRITICAL_SAFE(&mux);
	stream.printf("# light=%u,dropped=%u\nid,us,event,key,value,detail\n", unsigned(lightIndex), dropped);
	for(unsigned i = 0; i < count; ++i) {
		const auto& entry = entries[i];
		stream.printf("%u,%lld,%u,%lu,%lu,%lu\n", i + 1, (long long)entry.us, unsigned(entry.event),
					  (unsigned long)entry.key, (unsigned long)entry.value, (unsigned long)entry.detail);
	}
}

void IRAM_ATTR hwPwmTimingTrace(uint8_t event, uint8_t channel, uint32_t duty, uint32_t duration)
{
	FadeTiming::record(static_cast<FadeTiming::Event>(event), channel, duty, duration);
}
#endif
