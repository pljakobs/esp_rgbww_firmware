#pragma once

#include <cstdint>

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

bool start();
Snapshot snapshot();

} // namespace EventQueueMonitor
