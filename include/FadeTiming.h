#pragma once

#include <cstdint>

#ifndef FADE_TIMING_DIAGNOSTICS
#define FADE_TIMING_DIAGNOSTICS 0
#endif

class MemoryDataStream;

namespace FadeTiming
{
enum class Event : uint8_t {
	HwBegin = 0, HwStarted = 1, HwDone = 2, HwDispatched = 3, HwCycles = 4, DutyWritten = 5,
	Command = 10, Accepted = 11, Show = 12, State = 13, Rendered = 14, Send = 15, Sent = 16
};

#if FADE_TIMING_DIAGNOSTICS && defined(ARCH_ESP32)
#define FADE_TIMING_ONLY(...) __VA_ARGS__
#define FADE_TIMING_SAMPLE(light, ...) \
	(FadeTiming::selected(light) ? FadeTiming::record(__VA_ARGS__) : 0u)
#define FADE_TIMING_LIGHT(light, ...) \
	do { if(FadeTiming::selected(light)) { __VA_ARGS__; } } while(false)
uint32_t record(Event event, uint32_t key, uint32_t value = 0, uint32_t detail = 0);
bool selected(uint16_t light);
void start(uint16_t light);
void exportCsv(MemoryDataStream& stream);
#else
#define FADE_TIMING_ONLY(...)
#define FADE_TIMING_SAMPLE(light, ...) 0u
#define FADE_TIMING_LIGHT(light, ...) ((void)0)
inline uint32_t record(Event, uint32_t, uint32_t = 0, uint32_t = 0) { return 0; }
inline bool selected(uint16_t) { return false; }
#endif
}
