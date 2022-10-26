/**
 * @file
 * @author  Patrick Jahns http://github.com/patrickjahns
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

#ifdef __riscv
//#if SMING_SOC==esp32c3
//#warning "redefining INT32 to be int, not long int for riscv based esp32c3"
#undef __INT32_TYPE__
#define __INT32_TYPE__      int

#undef __UINT32_TYPE__
#define __UINT32_TYPE__     unsigned int

#endif // __riscv

#include <RGBWWCtrl.h>

Application app;

// Sming Framework INIT method - called during boot
void GDB_IRAM_ATTR init() {

    Serial.begin(SERIAL_BAUD_RATE); // 115200 by default
    Serial.systemDebugOutput(true); // Debug output to serial
    //System.setCpuFrequencye(CF_160MHz);

    // set CLR pin to input
    pinMode(CLEAR_PIN, INPUT);

    // seperated application init
    app.init();

    // Run Services on system ready
    System.onReady(SystemReadyDelegate(&Application::startServices, &app));
}

Application::~Application() {
    if (pNtpclient != nullptr) {
        delete pNtpclient;
        pNtpclient = nullptr;
    }
}

void Application::uptimeCounter() {
    ++_uptimeMinutes;
}

void Application::init() {
    debug_i("RGBWW Controller v %s\r\n", fw_git_version);

    //load settings
    _uptimetimer.initializeMs(60000, TimerDelegate(&Application::uptimeCounter, this)).start();

#ifdef ARCH_ESP8266
    // load boot information
    uint8 bootmode, bootslot;
    if (rboot_get_last_boot_mode(&bootmode)) {
        if (bootmode == MODE_TEMP_ROM) {
            debug_i("Application::init - booting after OTA");
        } else {
            debug_i("Application::init - normal boot");
        }
        _bootmode = bootmode;
    }

    if (rboot_get_last_boot_rom(&bootslot)) {
        _romslot = bootslot;
    }
#endif

    // mount filesystem
    mountfs(getRomSlot());

    // check if we need to reset settings
    if (digitalRead(CLEAR_PIN) < 1) {
        debug_i("CLR button low - resetting settings");
        cfg.reset();
        network.forgetWifi();
    }

    // check ota
#ifdef ARCH_ESP8266
    ota.checkAtBoot();
#endif

    // load config
    if (cfg.exist()) {
        cfg.load();
    } else {
        debug_i("Application::init - first run");
        _first_run = true;
        cfg.save();
    }

    mqttclient.init();

    // initialize led ctrl
    rgbwwctrl.init();

    initButtons();

    // initialize networking
    network.init();

    // initialize webserver
    app.webserver.init();

    if (cfg.ntp.enabled) {
        String server = cfg.ntp.server.length() > 0 ? cfg.ntp.server : NTP_DEFAULT_SERVER;
        unsigned interval = cfg.ntp.interval > 0 ? cfg.ntp.interval : NTP_DEFAULT_AUTOQUERY_SECONDS;
        debug_i("Enabling NTP server '%s' with interval %d s", server.c_str(), interval);
        pNtpclient = new NtpClient(server, interval);
    }
    else {
        debug_i("Disabling NTP server");
    }
}

void Application::initButtons() {
    if (cfg.general.buttons_config.length() <= 0)
        return;

    debug_i("Configuring buttons using string: '%s'", cfg.general.buttons_config.c_str());

    Vector<String> buttons;
    splitString(cfg.general.buttons_config, ',', buttons);

    for(int i=0; i < buttons.count(); ++i) {
        if (buttons[i].length() == 0)
            continue;

        int pin = buttons[i].toInt();
        if (pin >= _lastToggles.size()) {
            debug_i("Pin %d is invalid. Max is %d", pin, _lastToggles.size() - 1);
            continue;
        }
        debug_i("Configuring button: '%s'", buttons[i].c_str());

        _lastToggles[pin] = 0ul;

        attachInterrupt(pin,  std::bind(&Application::onButtonTogglePressed, this, pin), FALLING);
        pinMode(pin, INPUT_PULLUP);
    }
}

// Will be called when system initialization was completed
void Application::startServices() {
    debug_i("Application::startServices");
    rgbwwctrl.start();
    webserver.start();

    if (cfg.events.server_enabled)
        eventserver.start();
}

void Application::restart() {
    debug_i("Application::restart");
    if (network.isApActive()) {
        network.stopAp();
        _systimer.initializeMs(500, TimerDelegate(&Application::restart, this)).startOnce();
    }
    System.restart();
}

void Application::reset() {
    debug_i("Application::reset");
    cfg.reset();
    rgbwwctrl.colorReset();
    network.forgetWifi();
    delay(500);
    restart();
}

bool Application::delayedCMD(String cmd, int delay) {
    debug_i("Application::delayedCMD cmd: %s - delay: %i", cmd.c_str(), delay);
    if (cmd.equals("reset")) {
        _systimer.initializeMs(delay, TimerDelegate(&Application::reset, this)).startOnce();
    } else if (cmd.equals("restart")) {
        _systimer.initializeMs(delay, TimerDelegate(&Application::restart, this)).startOnce();
    } else if (cmd.equals("stopap")) {
        network.stopAp(2000);
    } else if (cmd.equals("forget_wifi")) {
        _systimer.initializeMs(delay, TimerDelegate(&AppWIFI::forgetWifi, &network)).startOnce();
    } else if (cmd.equals("switch_rom")) {
        switchRom();
        _systimer.initializeMs(delay, TimerDelegate(&Application::restart, this)).startOnce();
    } else {
        return false;
    }
    return true;
}

void Application::mountfs(int slot) {
   	debug_i("Application::mountfs rom slot: %i", slot);
	String name = F("spiffs_");
   	name += String(slot);
    auto part = Storage::findPartition(name);
    spiffs_mount(part);
    /*
    if (slot == 0) {
        debug_i("Application::mountfs trying to mount spiffs at %x, length %d",
                RBOOT_SPIFFS_0, SPIFF_SIZE);
        spiffs_mount_manual(RBOOT_SPIFFS_0, SPIFF_SIZE);
    } else {
        debug_i("Application::mountfs trying to mount spiffs at %x, length %d",
                RBOOT_SPIFFS_1, SPIFF_SIZE);
        spiffs_mount_manual(RBOOT_SPIFFS_1, SPIFF_SIZE);
    }*/

    _fs_mounted = true;
}

void Application::umountfs() {
    debug_i("Application::umountfs");
    //spiffs_unmount();
    fileFreeFileSystem();
    _fs_mounted = false;
}

void Application::switchRom() {
    debug_i("Application::switchRom");
    int slot = getRomSlot();
    if (slot == 0) {
        slot = 1;
    } else {
        slot = 0;
    }
#ifdef ARCH_ESP8266
    rboot_set_current_rom(slot);
#endif
}

void Application::onWifiConnected(const String& ssid) {
    debug_i("Application::onWifiConnected");
}

void Application::onCommandRelay(const String& method, const JsonObject& params) {
    if (!cfg.sync.cmd_master_enabled)
        return;

    mqttclient.publishCommand(method, params);
}

void Application::onButtonTogglePressed(int pin) {
    unsigned long now = millis();
    unsigned long diff = now - _lastToggles[pin];
    if (diff > cfg.general.buttons_debounce_ms) {  // debounce
        debug_i("Button %d pressed - toggle", pin);
        rgbwwctrl.toggle();
        _lastToggles[pin] = now;
    }
    else {
        debug_d("Button press ignored by debounce. Diff: %d Debounce: %d", diff, cfg.general.buttons_debounce_ms);
    }
}

uint32_t Application::getUptime() {
    return _uptimeMinutes * 60u;
}
