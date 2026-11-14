#include <FadeTiming.h>

#if FADE_TIMING_DIAGNOSTICS && defined(ARCH_ESP32)
#include <SmingCore.h>
#include <Data/Stream/MemoryDataStream.h>
#include <esp_timer.h>
#include <esp_private/esp_clk.h>
#include <esp_tasks.h>
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
constexpr unsigned Capacity = 2048;
Entry entries[Capacity];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
bool recording;
bool serviceRecording;
std::atomic<uint16_t> lightIndex{0};
unsigned count;
unsigned dropped;
struct Timing {
	uint64_t total{};
	uint32_t count{};
	uint32_t maximum{};
	void IRAM_ATTR add(uint32_t us)
	{
		total += us;
		++count;
		if(us > maximum) maximum = us;
	}
};
struct Accounting {
	Timing service;
	Timing lockWait;
	Timing queueWait;
	Timing dispatch;
	uint32_t rejected{};
};
Accounting accounting;
bool accountingEnabled;
int64_t queuedAt;
#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
TaskStatus_t taskSnapshot[32];
#endif
}

// Prevent compiler-generated dispatch tables from residing in flash.
uint32_t IRAM_ATTR __attribute__((optimize("no-jump-tables", "no-tree-switch-conversion")))
FadeTiming::record(Event event, uint32_t key, uint32_t value, uint32_t detail)
{
	// Compiler atomics keep ISR flag reads inline even in size-optimized builds.
	if(__atomic_load_n(&accountingEnabled, __ATOMIC_RELAXED) && unsigned(event) >= 20) {
		portENTER_CRITICAL_SAFE(&mux);
		if(__atomic_load_n(&accountingEnabled, __ATOMIC_RELAXED)) {
			switch(unsigned(event)) {
			case 21: accounting.lockWait.add(value); accounting.service.add(detail); break;
			case 24: queuedAt = esp_timer_get_time(); break;
			case 25:
				if(queuedAt != 0) accounting.queueWait.add(esp_timer_get_time() - queuedAt);
				queuedAt = 0;
				break;
			case 26: accounting.dispatch.add(value); break;
			case 27: ++accounting.rejected; queuedAt = 0; break;
			}
		}
		portEXIT_CRITICAL_SAFE(&mux);
	}
	if(!__atomic_load_n(&recording, __ATOMIC_RELAXED)) return 0;
	if(__atomic_load_n(&serviceRecording, __ATOMIC_RELAXED) && unsigned(event) < 10) return 0;
	portENTER_CRITICAL_SAFE(&mux);
	uint32_t id = 0;
	if(__atomic_load_n(&recording, __ATOMIC_RELAXED)) {
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
	return __atomic_load_n(&recording, __ATOMIC_RELAXED) && light == lightIndex.load(std::memory_order_relaxed);
}

bool FadeTiming::serviceEnabled()
{
	return __atomic_load_n(&accountingEnabled, __ATOMIC_RELAXED) ||
		   (__atomic_load_n(&recording, __ATOMIC_RELAXED) && __atomic_load_n(&serviceRecording, __ATOMIC_RELAXED));
}

void FadeTiming::start(uint16_t light, bool service)
{
	portENTER_CRITICAL_SAFE(&mux);
	__atomic_store_n(&recording, false, __ATOMIC_RELAXED);
	count = dropped = 0;
	lightIndex.store(light, std::memory_order_relaxed);
	__atomic_store_n(&serviceRecording, service, __ATOMIC_RELAXED);
	__atomic_store_n(&recording, true, __ATOMIC_RELAXED);
	portEXIT_CRITICAL_SAFE(&mux);
}

void FadeTiming::exportCsv(MemoryDataStream& stream)
{
	portENTER_CRITICAL_SAFE(&mux);
	__atomic_store_n(&recording, false, __ATOMIC_RELAXED);
	portEXIT_CRITICAL_SAFE(&mux);
	stream.printf("# light=%u,dropped=%u\nid,us,event,key,value,detail\n",
				  unsigned(lightIndex.load(std::memory_order_relaxed)), dropped);
	for(unsigned i = 0; i < count; ++i) {
		const auto& entry = entries[i];
		stream.printf("%u,%lld,%u,%lu,%lu,%lu\n", i + 1, (long long)entry.us, unsigned(entry.event),
					  (unsigned long)entry.key, (unsigned long)entry.value, (unsigned long)entry.detail);
	}
}

bool FadeTiming::exportTasks(MemoryDataStream& stream, bool account)
{
#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
	configRUN_TIME_COUNTER_TYPE total = 0;
	const auto size = uxTaskGetSystemState(taskSnapshot, 32, &total);
	if(size == 0) {
		debug_e("FadeTiming: task snapshot exceeds 32 tasks");
		return false;
	}
	sming_dispatch_stats_t dispatcher{};
	const bool haveDispatcher = sming_dispatch_stats_take(&dispatcher);
	portENTER_CRITICAL_SAFE(&mux);
	const auto sample = accounting;
	accounting = {};
	if(!__atomic_load_n(&accountingEnabled, __ATOMIC_RELAXED)) queuedAt = 0;
	__atomic_store_n(&accountingEnabled, account, __ATOMIC_RELAXED);
	portEXIT_CRITICAL_SAFE(&mux);
	stream.printf("# total_us=%llu,cores=%u,cpu_hz=%u",
				  static_cast<unsigned long long>(total), unsigned(portNUM_PROCESSORS),
				  unsigned(esp_clk_cpu_freq()));
	stream.printf(",dispatcher_available=%u,batch_count=%u,batch_total_us=%llu,batch_max_us=%u"
				  ",queue_peak=%u,event_full=%u,mbox_full=%u",
				  unsigned(haveDispatcher), unsigned(dispatcher.batches),
				  static_cast<unsigned long long>(dispatcher.batch_total_us),
				  unsigned(dispatcher.batch_max_us), unsigned(dispatcher.sampled_queue_peak),
				  unsigned(dispatcher.event_post_failures), unsigned(dispatcher.mailbox_post_failures));
	const auto writeTiming = [&stream](const char* name, const Timing& timing) {
		stream.printf(",%s_count=%u,%s_total_us=%llu,%s_max_us=%u", name, unsigned(timing.count),
					  name, static_cast<unsigned long long>(timing.total), name, unsigned(timing.maximum));
	};
	writeTiming("service", sample.service);
	writeTiming("lock", sample.lockWait);
	writeTiming("queue_wait", sample.queueWait);
	writeTiming("led_dispatch", sample.dispatch);
	stream.printf(",led_rejected=%u\nid,name,runtime_us,priority,core\n", unsigned(sample.rejected));
	for(unsigned i = 0; i < size; ++i) {
		const auto& task = taskSnapshot[i];
		int core = -1;
#if configTASKLIST_INCLUDE_COREID
		if(task.xCoreID >= 0 && task.xCoreID < portNUM_PROCESSORS) core = task.xCoreID;
#endif
		stream.printf("%u,%s,%llu,%u,%d\n", unsigned(task.xTaskNumber), task.pcTaskName,
					  static_cast<unsigned long long>(task.ulRunTimeCounter), unsigned(task.uxBasePriority), core);
	}
	return true;
#else
	debug_e("FadeTiming: ESP timer FreeRTOS runtime statistics are not enabled");
	return false;
#endif
}

void IRAM_ATTR hwPwmTimingTrace(uint8_t event, uint8_t channel, uint32_t duty, uint32_t duration)
{
	FadeTiming::record(static_cast<FadeTiming::Event>(event), channel, duty, duration);
}
#endif
