#include "EventQueueMonitor.h"

#include <RGBWWCtrl.h>
#include <SmingCore.h>

#ifdef ARCH_ESP32
#include <esp_timer.h>

namespace {
constexpr uint32_t PROBE_INTERVAL_MS = 1000;
constexpr uint32_t PROBE_TIMEOUT_MS = 5000;
constexpr uint32_t FAILURE_STREAK_THRESHOLD = 3;

portMUX_TYPE monitorMux = portMUX_INITIALIZER_UNLOCKED;
esp_timer_handle_t probeTimer = nullptr;
bool initialized = false;

struct MonitorState {
	bool available = false;
	bool stalled = false;
	bool pending = false;
	bool hasDispatched = false;
	uint32_t nextGeneration = 0;
	uint32_t pendingGeneration = 0;
	uint32_t pendingSince = 0;
	uint32_t lastDispatch = 0;
	uint32_t probes = 0;
	uint32_t dispatched = 0;
	uint32_t enqueueFailures = 0;
	uint32_t consecutiveFailures = 0;
	uint32_t stalls = 0;
};

MonitorState state;

void recordStall()
{
	if(!state.stalled) {
		state.stalled = true;
		++state.stalls;
	}
}

void probeDispatched(uint32_t generation)
{
	const uint32_t now = millis();
	portENTER_CRITICAL(&monitorMux);
	if(state.pending && state.pendingGeneration == generation) {
		state.pending = false;
		state.hasDispatched = true;
		state.lastDispatch = now;
		state.consecutiveFailures = 0;
		state.stalled = false;
		++state.dispatched;
	}
	portEXIT_CRITICAL(&monitorMux);
}

void probeTimerCallback(void*)
{
	const uint32_t now = millis();
	uint32_t generation;

	portENTER_CRITICAL(&monitorMux);
	if(state.pending) {
		if(now - state.pendingSince >= PROBE_TIMEOUT_MS) {
			recordStall();
		}
		portEXIT_CRITICAL(&monitorMux);
		return;
	}

	state.pending = true;
	state.pendingSince = now;
	generation = ++state.nextGeneration;
	state.pendingGeneration = generation;
	++state.probes;
	portEXIT_CRITICAL(&monitorMux);

	if(!System.queueCallback(probeDispatched, generation)) {
		portENTER_CRITICAL(&monitorMux);
		if(state.pending && state.pendingGeneration == generation) {
			state.pending = false;
			++state.enqueueFailures;
			++state.consecutiveFailures;
			if(state.consecutiveFailures >= FAILURE_STREAK_THRESHOLD) {
				recordStall();
			}
		}
		portEXIT_CRITICAL(&monitorMux);
	}
}
} // namespace
#endif

namespace EventQueueMonitor {

bool start()
{
#ifdef ARCH_ESP32
	if(initialized) {
		return state.available;
	}
	initialized = true;

	esp_timer_create_args_t timerArgs = {};
	timerArgs.callback = probeTimerCallback;
	timerArgs.name = "event_queue_probe";
	esp_err_t error = esp_timer_create(&timerArgs, &probeTimer);
	if(error != ESP_OK) {
		cdebug_e("EventQueueMonitor", "esp_timer_create failed: %s", esp_err_to_name(error));
		return false;
	}

	error = esp_timer_start_periodic(probeTimer, PROBE_INTERVAL_MS * 1000);
	if(error != ESP_OK) {
		cdebug_e("EventQueueMonitor", "esp_timer_start_periodic failed: %s", esp_err_to_name(error));
		esp_timer_delete(probeTimer);
		probeTimer = nullptr;
		return false;
	}

	portENTER_CRITICAL(&monitorMux);
	state.available = true;
	portEXIT_CRITICAL(&monitorMux);
	return true;
#else
	return false;
#endif
}

Snapshot snapshot()
{
	Snapshot result;
	const uint32_t now = millis();
#ifdef ARCH_ESP32
	portENTER_CRITICAL(&monitorMux);
	result.available = state.available;
	result.stalled = state.stalled;
	result.pending = state.pending;
	result.hasDispatched = state.hasDispatched;
	result.probes = state.probes;
	result.dispatched = state.dispatched;
	result.enqueueFailures = state.enqueueFailures;
	result.consecutiveFailures = state.consecutiveFailures;
	result.stalls = state.stalls;
	result.lastDispatchMsAgo = state.hasDispatched ? now - state.lastDispatch : 0;
	result.pendingMs = state.pending ? now - state.pendingSince : 0;
	portEXIT_CRITICAL(&monitorMux);
#else
	result.available = false;
	result.stalled = false;
	result.pending = false;
	result.hasDispatched = false;
	result.probes = 0;
	result.dispatched = 0;
	result.enqueueFailures = 0;
	result.consecutiveFailures = 0;
	result.stalls = 0;
	result.lastDispatchMsAgo = 0;
	result.pendingMs = 0;
#endif
	return result;
}

} // namespace EventQueueMonitor
