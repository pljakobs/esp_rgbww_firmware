#include <CallbackTiming.h>

#if EVENT_MONITOR && !defined(SMING_RELEASE)
#include <SmingCore.h>
#include <application.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>

namespace CallbackTiming
{
namespace
{
constexpr std::array<const char*, unsigned(Id::Count)> names{{
	"led_update", "light_show", "light_state", "current_state", "ws_pump", "light_reload",
	"ota_verify", "ota_hash", "runtime_info", "runtime_collect", "runtime_json", "runtime_enqueue",
	"telemetry", "buttons", "mqtt_receive", "timing_report",
	"root", "populate", "render", "normalize", "send", "state_publish",
}};
constexpr uint16_t noPath = UINT16_MAX;
constexpr size_t maxPaths = 96;
constexpr size_t maxDepth = 8;
constexpr size_t maxNameLength = 15;
static_assert([]() {
	for(const auto* name : names) {
		size_t length = 0;
		while(name[length] != '\0') {
			++length;
		}
		if(length > maxNameLength) {
			return false;
		}
	}
	return true;
}(), "Increase the timing path buffer when adding longer component names");
struct Path {
	Id id{};
	uint16_t parent{noPath};
	uint8_t depth{};
	Statistics statistics;
};
std::array<Path, maxPaths> paths;
std::array<Statistics, maxPaths> snapshot;
size_t pathCount;
Scope* activeScope;
uint32_t droppedScopes;
Timer reportTimer;
uint32_t lastReportMs;
uint32_t reportSequence;

uint16_t findPath(Id id, uint16_t parent)
{
	for(size_t i = 0; i < pathCount; ++i) {
		if(paths[i].id == id && paths[i].parent == parent) {
			return uint16_t(i);
		}
	}
	const unsigned depth = parent == noPath ? 1 : paths[parent].depth + 1;
	if(pathCount == maxPaths || depth > maxDepth) {
		return noPath;
	}
	const auto index = uint16_t(pathCount++);
	paths[index].id = id;
	paths[index].parent = parent;
	paths[index].depth = uint8_t(depth);
	return index;
}

void pathName(uint16_t index, char* buffer)
{
	std::array<Id, maxDepth> parts;
	size_t depth = 0;
	while(index != noPath) {
		parts[depth++] = paths[index].id;
		index = paths[index].parent;
	}
	buffer[0] = '\0';
	while(depth != 0) {
		if(buffer[0] != '\0') {
			strcat(buffer, "/");
		}
		strcat(buffer, names[unsigned(parts[--depth])]);
	}
}

#ifdef ARCH_ESP32
sming_dispatch_trace_t failureTrace;

void reportTraceEntry(uint32_t sequence, const char* state, uint32_t index,
					  const sming_dispatch_trace_entry_t& entry)
{
	app.udpSyslogStream.printf(
		"DispatcherTrace seq=%u state=%s index=%u batch=%u start_us=%llu elapsed_us=%u sig=0x%08x par=0x%08x\n",
		unsigned(sequence), state, unsigned(index), unsigned(entry.batch),
		static_cast<unsigned long long>(entry.started_us), unsigned(entry.elapsed_us),
		unsigned(entry.sig), unsigned(entry.par));
}
#endif

void report()
{
	const uint32_t now = millis();
	const uint32_t windowMs = now - lastReportMs;
	lastReportMs = now;
	for(size_t i = 0; i < pathCount; ++i) {
		snapshot[i] = paths[i].statistics;
		paths[i].statistics = {};
	}
	const auto count = pathCount;
	const auto dropped = droppedScopes;
	droppedScopes = 0;
	Scope timing(Id::Report);
#ifdef ARCH_ESP32
	sming_dispatch_stats_t dispatcher{};
	const bool haveDispatcher = sming_dispatch_stats_take(&dispatcher);
	const bool haveTrace = sming_dispatch_trace_take(&failureTrace);
#endif

	std::array<size_t, maxPaths> order;
	for(size_t i = 0; i < count; ++i) {
		order[i] = i;
	}
	std::sort(order.begin(), order.begin() + count, [&](size_t a, size_t b) {
		const auto& left = snapshot[a];
		const auto& right = snapshot[b];
		if(left.averageUs() != right.averageUs()) {
			return left.averageUs() > right.averageUs();
		}
		return left.totalUs > right.totalUs;
	});

	const auto sequence = ++reportSequence;
	app.udpSyslogStream.printf(
		"CallbackTiming seq=%u window_ms=%u inclusive=1 order=avg_us format=2 dropped_scopes=%u\n",
		unsigned(sequence), unsigned(windowMs), unsigned(dropped));
#ifdef ARCH_ESP32
	if(haveDispatcher) {
		app.udpSyslogStream.printf(
			"DispatcherTiming seq=%u batches=%u events=%u avg_us=%.1f max_us=%u total_us=%llu "
			"max_events=%u queue_peak_sampled=%u event_full=%u mbox_attempts=%u mbox_full=%u\n",
			unsigned(sequence), unsigned(dispatcher.batches), unsigned(dispatcher.events),
			dispatcher.batches ? double(dispatcher.batch_total_us) / dispatcher.batches : 0.0,
			unsigned(dispatcher.batch_max_us), static_cast<unsigned long long>(dispatcher.batch_total_us),
			unsigned(dispatcher.batch_max_events), unsigned(dispatcher.sampled_queue_peak),
			unsigned(dispatcher.event_post_failures), unsigned(dispatcher.mailbox_post_attempts),
			unsigned(dispatcher.mailbox_post_failures));
	}
	if(haveTrace) {
		app.udpSyslogStream.printf(
			"DispatcherTrace seq=%u failed_us=%llu queue_depth=%u failures=%u completed=%u overwritten=%u active=%u\n",
			unsigned(sequence), static_cast<unsigned long long>(failureTrace.failed_us),
			unsigned(failureTrace.queue_depth), unsigned(failureTrace.failures), unsigned(failureTrace.count),
			unsigned(failureTrace.overwritten), unsigned(failureTrace.active));
		for(uint32_t i = 0; i < failureTrace.count; ++i) {
			reportTraceEntry(sequence, "completed", i, failureTrace.entries[i]);
		}
		if(failureTrace.active) {
			reportTraceEntry(sequence, "active", failureTrace.count, failureTrace.current);
		}
	}
#endif
	for(size_t i = 0; i < count; ++i) {
		const auto index = order[i];
		const auto& entry = snapshot[index];
		if(entry.calls == 0) {
			continue;
		}
		char name[maxDepth * (maxNameLength + 1)];
		pathName(uint16_t(index), name);
		app.udpSyslogStream.printf(
			"CallbackTiming seq=%u format=2 name=%s calls=%u avg_us=%.1f max_us=%u total_us=%llu\n",
			unsigned(sequence), name, unsigned(entry.calls), entry.averageUs(), unsigned(entry.maxUs),
			static_cast<unsigned long long>(entry.totalUs));
	}
}
} // namespace

uint32_t nowUs()
{
	return micros();
}

Scope::Scope(Id id) : previous_(activeScope), path_(noPath), startedUs_(nowUs())
{
	assert(unsigned(id) < names.size());
	if(previous_ == nullptr || previous_->path_ != noPath) {
		path_ = findPath(id, previous_ == nullptr ? noPath : previous_->path_);
	}
	if(path_ == noPath) {
		++droppedScopes;
	}
	activeScope = this;
}

Scope::~Scope()
{
	const uint32_t elapsed = nowUs() - startedUs_;
	assert(activeScope == this);
	activeScope = previous_;
	if(path_ != noPath) {
		paths[path_].statistics.add(elapsed);
	}
}

void start()
{
	lastReportMs = millis();
	reportTimer.initializeMs<5000>(report).start();
}
} // namespace CallbackTiming
#endif
