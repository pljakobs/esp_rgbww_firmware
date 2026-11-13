#pragma once

#include <cstdint>

#ifndef EVENT_MONITOR
#define EVENT_MONITOR 0
#endif

namespace CallbackTiming
{
enum class Id : uint8_t {
	LedUpdate,
	LightShow,
	LightState,
	CurrentState,
	WebsocketPump,
	LightReload,
	OtaVerify,
	OtaHash,
	RuntimeInfo,
	RuntimeCollect,
	RuntimeJson,
	RuntimeEnqueue,
	Telemetry,
	Buttons,
	MqttReceive,
	Report,
	Root,
	Populate,
	Render,
	Normalize,
	Send,
	Count
};

struct Statistics {
	uint64_t totalUs{};
	uint32_t calls{};
	uint32_t maxUs{};

	void add(uint32_t elapsedUs)
	{
		totalUs += elapsedUs;
		++calls;
		if(elapsedUs > maxUs) {
			maxUs = elapsedUs;
		}
	}

	double averageUs() const
	{
		return calls == 0 ? 0.0 : double(totalUs) / calls;
	}
};

#if EVENT_MONITOR && !defined(SMING_RELEASE)
#define CALLBACK_TIMING_SCOPE(name, id) CallbackTiming::Scope name(CallbackTiming::Id::id)
#define CALLBACK_TIMING_START() CallbackTiming::start()
uint32_t nowUs();
void start();

// Same-thread synchronous scopes only, never ISR context. ESP32 network callbacks
// and the Sming dispatcher both run on the TCP/IP thread.
class Scope
{
public:
	explicit Scope(Id id);
	~Scope();

	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
	Scope(Scope&&) = delete;
	Scope& operator=(Scope&&) = delete;

private:
	Scope* previous_;
	uint16_t path_;
	uint32_t startedUs_;
};
#else
#define CALLBACK_TIMING_SCOPE(name, id) ((void)0)
#define CALLBACK_TIMING_START() ((void)0)
inline void start()
{
}

class Scope
{
public:
	explicit Scope(Id)
	{
	}
};
#endif
} // namespace CallbackTiming
