#pragma once

#include <cstdint>

#ifdef ARCH_ESP32
#include <esp_tasks.h>
#endif

uint32_t micros();
uint32_t millis();

class Timer
{
public:
	template <uint32_t milliseconds> Timer& initializeMs(void (*handler)())
	{
		intervalMs = milliseconds;
		callback = handler;
		return *this;
	}

	void start()
	{
		periodic = true;
	}

	static uint32_t intervalMs;
	static void (*callback)();
	static bool periodic;
};
