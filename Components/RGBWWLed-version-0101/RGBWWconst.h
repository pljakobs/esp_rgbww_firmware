/**
 * RGBWWLed - simple Library for controlling RGB WarmWhite ColdWhite LEDs via PWM
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
 *          Peter Jakobs    http://github.com/pljakobs
 *
 * All files of this project are provided under the LGPL v3 license.
 */

#pragma once

#include <SmingCore.h>
#include <pgmspace.h>
#include <stdint.h>
#include <stddef.h>
#include <array>
#include <utility>

#ifdef SMING_VERSION
#define RGBWW_USE_ESP_HWPWM
#define RGBWW_PWMRESOLUTION 65536
#define RGBWW_CALC_DEPTH 10
#else
#define RGBWW_PWMRESOLUTION 1023
#define RGBWW_CALC_DEPTH 8
#endif

#define RGBWW_VERSION "0.10.1"
#define RGBWW_CALC_WIDTH int(pow(2, RGBWW_CALC_DEPTH))
#define RGBWW_CALC_MAXVAL int(RGBWW_CALC_WIDTH - 1)
#define RGBWW_CALC_HUEWHEELMAX int(RGBWW_CALC_MAXVAL * 6)

#define RGBWW_UPDATEFREQUENCY 50
#define RGBWW_MINTIMEDIFF  int(1000 / RGBWW_UPDATEFREQUENCY)
#define RGBWW_MINTIMEDIFF_US  RGBWW_MINTIMEDIFF * 1000
#ifdef ARCH_ESP8266
#define RGBWW_ANIMATIONQSIZE 20
#else
#define RGBWW_ANIMATIONQSIZE 100
#endif
#define RGBWW_WARMWHITEKELVIN 2700
#define RGBWW_COLDWHITEKELVIN 6000


// Determine Array Size and Return Type based on configurations
constexpr size_t TableSize = (RGBWW_CALC_DEPTH == 8) ? 256 : 1024;
using TableType = typename std::conditional<(RGBWW_CALC_DEPTH == 8 && RGBWW_PWMRESOLUTION == 256), uint8_t, uint16_t>::type;

// =============================================
// 1. Core Mathematical Curve Engines
//    these may or may not be problematic
//    when using Clang
//    tables with 1024 elements should be fine
//    larger tables may need to be rewritten
//    to avoid constexpr evaluation issues
//    also, Clang may complaint about the
//    constexpr evaluation of constexpr math
//    functions not being 100% accurate
// =============================================

// CIE 1931 Luminance Math
constexpr TableType calculate_cie_point(int index) {
    // Explicitly enforce double literals to prevent integer truncation
    double x = static_cast<double>(index) / 1023.0;
    double y = (x > 0.08) ? ((x + 0.16) / 1.16) * ((x + 0.16) / 1.16) * ((x + 0.16) / 1.16)
                          : (x / 9.03296);
    return static_cast<TableType>((y * 65535.0) + 0.5);
}

// Cubic Power Math
constexpr TableType calculate_cubic_point(int index) {
    double x = static_cast<double>(index) / 1023.0;
    double y = x * x * x;
    return static_cast<TableType>((y * 65535.0) + 0.5);
}

// Router function selecting formula based on compilation parameters
constexpr TableType generate_point(int index) {
    // Rely on safe constexpr evaluation instead of macro state variations
    if (TableSize == 1024 && RGBWW_PWMRESOLUTION == 65536) {
        return calculate_cie_point(index);
    } else {
        return calculate_cubic_point(index);
    }
}

// ==========================================
// 2. Compile-time Array Sequence Generator
// ==========================================
template<typename T, size_t... Is>
constexpr std::array<T, sizeof...(Is)> makeLookupTable(std::index_sequence<Is...>) {
    return {{ generate_point(Is)... }};
}

// ==========================================
// 3. Final Unified Interface
// ==========================================
// Replaces all 4 hardcoded options with a single, auto-calculating array
constexpr std::array<TableType, TableSize> RGBWW_dim_curve PROGMEM =
    makeLookupTable<TableType>(std::make_index_sequence<TableSize>{});

// ==========================================
// 4. Compile-Time Assertions (Zero-Cost Verification)
// ==========================================

#if RGBWW_CALC_DEPTH == 8
    #if RGBWW_PWMRESOLUTION == 256
        static_assert(RGBWW_dim_curve[0]   == 0,   "Data verification failed at index 0");
        static_assert(RGBWW_dim_curve[255] == 255, "Data verification failed at index 255");
    #else
        static_assert(RGBWW_dim_curve[0]   == 0,    "Data verification failed at index 0");
        static_assert(RGBWW_dim_curve[255] == 1023, "Data verification failed at index 255");
    #endif
#endif

#if RGBWW_CALC_DEPTH == 10
    #if RGBWW_PWMRESOLUTION == 65536
        static_assert(RGBWW_dim_curve[0]    == 0,     "Data verification failed at index 0");
        static_assert(RGBWW_dim_curve[1023] == 65535, "Data verification failed at index 1023");
    #else
        static_assert(RGBWW_dim_curve[0]    == 0,    "Data verification failed at index 0");
        static_assert(RGBWW_dim_curve[1023] == 1023, "Data verification failed at index 1023");
    #endif
#endif