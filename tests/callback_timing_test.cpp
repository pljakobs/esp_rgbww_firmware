#include <CallbackTiming.h>
#include <SmingCore.h>
#include <application.h>

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <limits>

uint32_t Timer::intervalMs;
void (*Timer::callback)();
bool Timer::periodic;
TestApplication app;

#ifdef ARCH_ESP32
static unsigned traceTakes;

bool sming_dispatch_stats_take(sming_dispatch_stats_t* stats)
{
	*stats = {};
	stats->mailbox_post_failures = 2;
	return true;
}

bool sming_dispatch_trace_take(sming_dispatch_trace_t* trace)
{
	if(++traceTakes != 1) {
		return false;
	}
	*trace = {};
	trace->failed_us = 1000;
	trace->queue_depth = 3;
	trace->failures = 2;
	trace->count = 2;
	trace->overwritten = 5;
	trace->active = true;
	trace->entries[0] = {100, 20, 1, 0x42001000, 0x1234};
	trace->entries[1] = {200, 30, 2, 0x42002000, 0x5678};
	trace->current = {900, 100, 3, 0x42003000, 0x9abc};
	return true;
}
#endif

namespace
{
uint32_t clockUs;
uint32_t clockMs;
}

uint32_t micros()
{
	return clockUs;
}

uint32_t millis()
{
	return clockMs;
}

void TestSyslogStream::printf(const char* format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	const int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	assert(length >= 0 && unsigned(length) < sizeof(buffer));
	output.append(buffer, size_t(length));
	clockUs += 10;
}

static void earlyReturn()
{
	CallbackTiming::Scope timing(CallbackTiming::Id::LightReload);
	clockUs += 7;
	return;
}

static void deepScopes(unsigned depth)
{
	CallbackTiming::Scope timing(CallbackTiming::Id::Root);
	if(depth > 1) {
		deepScopes(depth - 1);
	}
	clockUs += 1;
}

static void manyPaths(unsigned depth, CallbackTiming::Id id)
{
	CallbackTiming::Scope timing(id);
	if(depth > 1) {
		manyPaths(depth - 1, CallbackTiming::Id::Render);
		manyPaths(depth - 1, CallbackTiming::Id::Normalize);
	}
	clockUs += 1;
}

int main()
{
	using namespace CallbackTiming;
	Statistics stats;
	assert(stats.averageUs() == 0);
	stats.add(10);
	stats.add(31);
	assert(stats.calls == 2 && stats.totalUs == 41 && stats.maxUs == 31);
	assert(stats.averageUs() == 20.5);
	stats = {};
	stats.add(std::numeric_limits<uint32_t>::max());
	stats.add(std::numeric_limits<uint32_t>::max());
	assert(stats.totalUs == uint64_t(std::numeric_limits<uint32_t>::max()) * 2);

	start();
	assert(Timer::periodic && Timer::intervalMs == 5000 && Timer::callback != nullptr);
	{
		Scope outer(Id::LedUpdate);
		clockUs += 10;
		{
			Scope inner(Id::LightShow);
			clockUs += 20;
		}
		clockUs += 10;
	}
	{
		Scope timing(Id::LedUpdate);
		clockUs += 10;
	}
	earlyReturn();
	for(auto id : {Id::RuntimeCollect, Id::RuntimeJson, Id::RuntimeEnqueue}) {
		Scope timing(id);
		clockUs += 1;
	}
	clockUs = std::numeric_limits<uint32_t>::max() - 3;
	{
		Scope timing(Id::OtaHash);
		clockUs += 9;
	}
	clockMs = 5000;
	Timer::callback();
	const auto& output = app.udpSyslogStream.output;
	assert(output.find("seq=1 window_ms=5000 inclusive=1 order=avg_us") != std::string::npos);
	assert(output.find("name=led_update calls=2 avg_us=25.0 max_us=40 total_us=50") != std::string::npos);
	assert(output.find("name=led_update/light_show calls=1 avg_us=20.0 max_us=20 total_us=20") != std::string::npos);
	assert(output.find("name=light_reload calls=1 avg_us=7.0 max_us=7 total_us=7") != std::string::npos);
	assert(output.find("name=ota_hash calls=1 avg_us=9.0 max_us=9 total_us=9") != std::string::npos);
	assert(output.find("format=2 dropped_scopes=0") != std::string::npos);
	assert(output.find("name=led_update calls") < output.find("name=led_update/light_show"));
	assert(output.find("name=led_update/light_show") < output.find("name=ota_hash"));
	assert(output.find("name=ota_hash") < output.find("name=light_reload"));
	assert(output.find("name=ws_pump") == std::string::npos);
	assert(output.find("name=timing_report") == std::string::npos);
	for(const auto* name : {"runtime_collect", "runtime_json", "runtime_enqueue"}) {
		assert(output.find(std::string("name=") + name + " calls=1 avg_us=1.0 max_us=1 total_us=1") != std::string::npos);
	}
#ifdef ARCH_ESP32
	assert(output.find("mbox_full=2") != std::string::npos);
	assert(output.find("failed_us=1000 queue_depth=3 failures=2 completed=2 overwritten=5 active=1") != std::string::npos);
	assert(output.find("state=completed index=0 batch=1 start_us=100 elapsed_us=20 sig=0x42001000 par=0x00001234")
		   != std::string::npos);
	assert(output.find("state=completed index=1") > output.find("state=completed index=0"));
	assert(output.find("state=active index=2 batch=3 start_us=900 elapsed_us=100 sig=0x42003000 par=0x00009abc")
		   != std::string::npos);
#endif

	app.udpSyslogStream.output.clear();
	clockMs = 11000;
	Timer::callback();
	assert(output.find("seq=2 window_ms=6000") != std::string::npos);
	assert(output.find("name=led_update") == std::string::npos);
#ifdef ARCH_ESP32
	assert(traceTakes == 2);
	assert(output.find("DispatcherTrace") == std::string::npos);
	assert(output.find("name=timing_report calls=1 avg_us=130.0 max_us=130 total_us=130") != std::string::npos);
#else
	assert(output.find("name=timing_report calls=1 avg_us=80.0 max_us=80 total_us=80") != std::string::npos);
#endif

	app.udpSyslogStream.output.clear();
	{
		Scope outer(Id::LedUpdate);
		{
			Scope state(Id::LightState);
			{
				Scope root(Id::Root);
				clockUs += 2;
			}
			{
				Scope populate(Id::Populate);
				clockUs += 3;
			}
			{
				Scope render(Id::Render);
				clockUs += 4;
			}
			{
				Scope normalize(Id::Normalize);
				clockUs += 5;
			}
			{
				Scope send(Id::Send);
				Scope pump(Id::WebsocketPump);
				clockUs += 6;
			}
		}
		{
			Scope state(Id::CurrentState);
			Scope send(Id::Send);
			Scope pump(Id::WebsocketPump);
			clockUs += 8;
		}
	}
	{
		Scope pump(Id::WebsocketPump);
		clockUs += 10;
	}
	clockMs += 5000;
	Timer::callback();
	assert(output.find("name=led_update/light_state calls=1 avg_us=20.0") != std::string::npos);
	assert(output.find("name=led_update/current_state calls=1 avg_us=8.0") != std::string::npos);
	assert(output.find("name=led_update/light_state/send/ws_pump calls=1 avg_us=6.0") != std::string::npos);
	assert(output.find("name=led_update/current_state/send/ws_pump calls=1 avg_us=8.0") != std::string::npos);
	assert(output.find("name=ws_pump calls=1 avg_us=10.0") != std::string::npos);
	for(const auto* name : {"root", "populate", "render", "normalize", "send"}) {
		assert(output.find(std::string("name=led_update/light_state/") + name + " calls=1") != std::string::npos);
	}

	// A report during a parent scope must not fabricate a completed parent.
	app.udpSyslogStream.output.clear();
	{
		Scope outer(Id::RuntimeInfo);
		{
			Scope inner(Id::RuntimeCollect);
			clockUs += 3;
		}
		clockMs += 5000;
		Timer::callback();
		assert(output.find("name=runtime_info/runtime_collect calls=1") != std::string::npos);
		assert(output.find("name=runtime_info calls=") == std::string::npos);
	}
	app.udpSyslogStream.output.clear();
	clockMs += 5000;
	Timer::callback();
	assert(output.find("name=runtime_info calls=1") != std::string::npos);

	app.udpSyslogStream.output.clear();
	deepScopes(10);
	clockMs += 5000;
	Timer::callback();
	assert(output.find("dropped_scopes=2") != std::string::npos);
	assert(output.find("name=root/root/root/root/root/root/root/root calls=1") != std::string::npos);
	assert(output.find("name=root/root/root/root/root/root/root/root/root") == std::string::npos);

	app.udpSyslogStream.output.clear();
	manyPaths(8, Id::Telemetry);
	{
		Scope known(Id::LedUpdate);
		clockUs += 1;
	}
	clockMs += 5000;
	Timer::callback();
	const auto droppedPosition = output.find("dropped_scopes=");
	assert(droppedPosition != std::string::npos);
	unsigned dropped = 0;
	assert(sscanf(output.c_str() + droppedPosition, "dropped_scopes=%u", &dropped) == 1 && dropped > 0);
	assert(output.find("name=led_update calls=1 avg_us=1.0") != std::string::npos);
	app.udpSyslogStream.output.clear();
	clockMs += 5000;
	Timer::callback();
	assert(output.find("dropped_scopes=0") != std::string::npos);
}
