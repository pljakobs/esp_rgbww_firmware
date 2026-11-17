#pragma once
// Host-test stub for Arduino/Sming pgmspace.h — flash and RAM are the same here.
#include <stdint.h>

#define PROGMEM

#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))
