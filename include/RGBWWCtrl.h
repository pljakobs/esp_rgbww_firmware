/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
 *          Peter Jakobs http://github.com/pljakobs
 * 
 *
 * @section LICENSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details at
 * https://www.gnu.org/copyleft/gpl.html
 *
 * @section DESCRIPTION
 *
 *
 */

#pragma once

#if defined(SOC_ESP8266)
	#define SOC "esp8266"
#elif defined(SOC_ESP32S2)
	#define SOC "esp32s2"
#elif defined(SOC_ESP32S3)
	#define SOC "esp32s3"
#elif defined(SOC_ESP32C2)
	#define SOC "esp32c2"
#elif defined(SOC_ESP32C3)
	#define SOC "esp32c3"
#elif defined(SOC_ESP32)
    #define SOC "esp32"
#elif defined(ARCH_HOST)
    #define SOC "host"
#else
    #define SOC "unknown"
#endif

#if defined(ARCH_ESP32)
    #define HTTP_MAX_CONNECTIONS 10
#else
    #define HTTP_MAX_CONNECTIONS 4
#endif

#if defined(SMING_RELEASE)
    #define BUILD_TYPE "release"
#else
    #define BUILD_TYPE "debug"
#endif

//default defines

#if defined ARCH_ESP8266
#endif

#if defined ARCH_ESP32
    #undef CLEAR_PIN 
#endif

#define DEFAULT_AP_IP "192.168.4.1"
#define DEFAULT_AP_SECURED false
#define DEFAULT_AP_PASSWORD "rgbwwctrl"
#define DEFAULT_AP_SSIDPREFIX "Lightinator_"

#define DEFAULT_API_SECURED false
#define DEFAULT_API_PASSWORD "rgbwwctrl"
#define DEFAULT_CONNECTION_RETRIES 10
#define DEFAULT_OTA_URL "https://lightinator.de/version.json"

// RGBWW related
#define DEFAULT_COLORTEMP_WW 2700
#define DEFAULT_COLORTEMP_CW 6000

#ifdef ARCH_ESP8266
#define PWM_FREQUENCY 800
#elif ARCH_ESP32
#define PWM_FREQUENCY 4000
#elif ARCH_HOST
#define PWM_FREQUENCY 1000
#endif
#define RGBWW_USE_ESP_HWPWM

// output colors
#define ANSI_COLOR_WHITE "\033[37m"
#define ANSI_COLOR_RED "\033[31m"
#define ANSI_COLOR_GREEN "\033[32m"
#define ANSI_COLOR_YELLOW "\033[33m"
#define ANSI_COLOR_BLUE "\033[34m"
#define ANSI_COLOR_MAGENTA "\033[35m"
#define ANSI_COLOR_CYAN "\033[36m"
#define ANSI_COLOR_RESET "\033[0m"

// Debugging
#define DEBUG_APP 1

/*------------------------------------------------
|
| WebappOTA Constants
|
------------------------------------------------*/

#ifdef ARCH_HOST
    #define WEBAPP_OTA_MIN_UPDATE_HEAP 8000
#else
    #define WEBAPP_OTA_MIN_UPDATE_HEAP 15000
#endif

/*------------------------------------------------
|
| Crashloop recovery Constants
|
| Override any of the numbers from component.mk via -D... if desired.
|
------------------------------------------------*/

#ifndef CRASHLOOP_THRESHOLD
#define CRASHLOOP_THRESHOLD 5
#endif
#ifndef CRASHLOOP_HEALTHY_MS
#define CRASHLOOP_HEALTHY_MS 60000
#endif
#ifndef CRASHLOOP_MAX_SWITCHES
#define CRASHLOOP_MAX_SWITCHES 2
#endif
#define CRASHLOOP_MAGIC 0xC1A5107Du

#define CRASHLOOP_RTC_SLOT 128

/*------------------------------------------------
|
| crash reporting 
|
------------------------------------------------*/

#define CRASH_RTC_SLOT         68 // Moved from 64 to avoid rBoot collision
#define CRASH_RTC_MAGIC 0xDEADC0DEu
#define CRASH_RTC_MAGIC_OVERFLOW 0xBAD57AC0u
#define CRASH_STACK_WORDS 50


/*------------------------------------------------
|
| Host specific debugging
|
------------------------------------------------*/

#if ARCH_HOST
#include <malloc_count.h>
#define HOST_FREE_TARGET 15000
#endif



//includes
#include <RGBWWLed/RGBWWLed.h>
#if defined(ARCH_ESP8266) || defined(ESP32)
    #include <otaupdate.h>
#endif

#include <JsonObjectStream.h>
#include <ConfigDB/Json/Format.h>
#include <ConfigDB/Network/HttpImportResource.h>

#include <Data/CStringArray.h>
#include <Data/Format/Json.h>

#include "app-config.h"
#include "app-data.h"

#include <ledctrl.h>
#include <networking.h>
#include <webserver.h>
#include <mdnsHandler.h>
#include <telemetry.h>
#include <mqtt.h>
#include "jsonrpcmessage.h"
#include <rpccodec.h>
#include <eventserver.h>
#include <jsonprocessor.h>
#include <application.h>
#include <stepsync.h>
#include <arduinojson.h>
#include <controllers.h>

#define configDB_PATH "app-config"
#define dataDB_PATH "app-data"

#define RSYSLOG 1
