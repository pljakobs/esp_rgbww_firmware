/*
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
 *          Peter Jakobs http://github.com/pljakobs
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
 */
#pragma once
#include <RGBWWCtrl.h>
#include <otaupdate.h>
#include <controllers.h>
#include <mdnsHandler.h>
#include <udpSyslogStream.h>



static const char* fw_git_version = GITVERSION;
static const char* fw_git_date = GITDATE;
static const char* sming_git_version = SMING_VERSION;


// main forward declarations
class Application {

public:
    ~Application();

    void init();
    void initButtons();

    void startServices();
    void startNetworkServices();
    void stopServices();

    void reset();
    void restart();
    void forget_wifi_and_restart();
    bool delayedCMD(String cmd, int delay);

    void wsBroadcast(String message);
    void wsBroadcast(String cmd, String message);
    void wsBroadcast(const String& cmd, const JsonObject& params);

    void listSpiffsPartitions();
    
    bool mountfs(int slot);
    void umountfs();

    inline bool isFilesystemMounted() { return _fs_mounted; };
    inline bool isFirstRun() { return _first_run; };

    void checkRam();
#ifdef ARCH_ESP8266
    inline bool isTempBoot() { return _bootmode == MODE_TEMP_ROM; };
#else
    bool isTempBoot() { return false; };
#endif
    int getRomSlot();
    //inline int getBootMode() { return _bootmode; };
    void switchRom();

    void onCommandRelay(const String& method, const JsonObject& json);
    //void onWifiConnected(const String& ssid);
    
    void onButtonTogglePressed(int pin);

    uint32_t getUptime();
    void uptimeCounter();
    size_t getFreeHeapSize();
    bool checkHeap(size_t minHeap);
    size_t getMinimumHeapUptime() { return _minimumHeapUptime; }
    size_t getMinimumHeap10min() { return _minimumHeap10min; }

public:
    AppWIFI network;
    ApplicationWebserver webserver;
    UdpSyslogStream udpSyslogStream;
    APPLedCtrl rgbwwctrl;
    std::unique_ptr<Controllers> controllers;
    
    ApplicationOTA ota;
    std::unique_ptr<AppConfig> cfg;
    std::unique_ptr<AppData> data;


    
    EventServer eventserver;
    AppMqttClient mqttclient;
    TelemetryClient telemetryClient;
    JsonProcessor jsonproc;
    mdnsHandler mdnsService;
    NtpClient* pNtpclient = nullptr;

        // debug counters
    uint32_t _mDNS_received = 0;
    uint32_t _mDNS_replies = 0;
    struct rst_info* rtc_info;

    String sanitizeName(const String& input){
        String result = input;
        for (int i = 0; i < result.length(); i++) {
            if (result[i] == '_') {
                result[i] = '-';
            }
        }
        return result;
    }

    uint8_t getCpuPercent()
    {
        return cpuPercent;
    }

private:
    void loadbootinfo();
    void listFiles();
    void logRestart();
    void pollResetButton();

    Timer _systimer;
    int _bootmode = 0;
    int _romslot = 0;
    bool _first_run = false;
    bool _fs_mounted = false;
    bool _run_after_ota = false;

    uint8_t cpuPercent=0;

    Timer _uptimetimer;
    Timer _checkRamTimer;
    Timer _resetPinTimer;

    uint32_t _uptimeMinutes;
    size_t _minimumHeapUptime = 32768;
    size_t _minimumHeap10min=32768;

    std::array<int, 17> _lastToggles;

    uint32_t jsonrpc_id = 0;

    int8_t clearPin = 16; //  GPIO16 is the default for the old mrpj boards, newer boards will load from pinconfig 
    int8_t _clearPin = -1;

    bool _reboot_reported=false;
};
// forward declaration for global vars
extern Application app;
