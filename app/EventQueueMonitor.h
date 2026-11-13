#pragma once

#include <cstdint>

#ifndef EVENT_MONITOR
#define EVENT_MONITOR 0
#endif

namespace EventQueueMonitor {

struct Snapshot {
	bool available;
	bool stalled;
	bool pending;
	bool hasDispatched;
	uint32_t probes;
	uint32_t dispatched;
	uint32_t enqueueFailures;
	uint32_t consecutiveFailures;
	uint32_t stalls;
	uint32_t lastDispatchMsAgo;
	uint32_t pendingMs;
};

#if EVENT_MONITOR && defined(ARCH_ESP32) && !defined(SMING_RELEASE)
#define EVENT_QUEUE_MONITOR_START() EventQueueMonitor::start()
bool start();
Snapshot snapshot();
#else
#define EVENT_QUEUE_MONITOR_START() ((void)0)
inline bool start() { return false; }
inline Snapshot snapshot() { return {}; }
#endif

} // namespace EventQueueMonitor
