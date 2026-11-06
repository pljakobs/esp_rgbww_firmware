    
/**
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
 *
 */
#pragma once

#include "mqtt.h"
#include "stepsync.h"
#include "jsonprocessor.h"
#include <memory>
#include <vector>

#define APP_COLOR_FILE ".color"

enum PinCapability : uint8_t {
    CAPA_INPUT = 1u << 0,
    CAPA_OUTPUT = 1u << 1,
    CAPA_PWM = 1u << 2,
    CAPA_STRAPPING = 1u << 3,
    CAPA_PULL_UP = 1u << 4,
    CAPA_PULL_DOWN = 1u << 5,
    CAPA_RESERVED = 1u << 6,
};

struct PinConfig {
    PinConfig() : red(13), green(12), blue(14), warmwhite(5), coldwhite(4) {}

    int red;
    int green;
    int blue;
    int warmwhite;
    int coldwhite;
    bool isValid;
};

struct ConfiguredInput {
    String id;
    int gpio = -1;
    bool activeLow = true;
    bool internalPullUp = false;
    bool internalPullDown = false;
    uint16_t debounceMs = 50;
};

struct HardwareInputs {
    std::vector<ConfiguredInput> buttons;
    ConfiguredInput clear;
    bool i2cEnabled = false;
    int sda = -1;
    int scl = -1;
    uint32_t frequency = 100000;
};

class APPLedCtrl: public RGBWWLed {

public:
    virtual ~APPLedCtrl();

    void init();
    bool reload(String& error);
    bool resolveInputs(HardwareInputs& inputs, String& error) const;
    void setup();

    void start();
    void stop();
    void colorSave();
    void colorReset();
    void testChannels();
    void toggle();

    // SetOn/SetOff with ramp, queue, and channel support
    void setOn(const RGBWWLed::ChannelList& channels, int direction, const RampTimeOrSpeed& ramp, QueuePolicy queue, bool requeue, const String& name);
    void setOff(const RGBWWLed::ChannelList& channels, int direction, const RampTimeOrSpeed& ramp, QueuePolicy queue, bool requeue, const String& name);

    void updateLed();
    void onMasterClock(uint32_t steps);
    void onMasterClockReset();
    virtual void onAnimationFinished(const String& name, bool requeued);

    void setClockMaster(bool master, uint32_t interval = 0) {
        clockMaster = master;
        clockMasterInterval = interval;
    };
    void setColorMaster(bool master) {
        colorMaster = master;
    };
    void setTransitionInterval(uint32_t interval) {
        transFinInterval = interval;
    };
    void setColorInterval(uint32_t interval) {
        colorMasterInterval = interval;
    };
    void setColorMinInterval(uint32_t interval) {
        colorMinInterval = interval;
    };
    void setStartupColorLast(bool last) {
        startupColorLast = last;
    };
    void reconfigure();
private:
    void requestReload();
    String configurationSignature() const;
    bool _reloadQueued = false;
    bool _watchingConfiguration = false;
    std::shared_ptr<uint8_t> _reloadLifetime = std::make_shared<uint8_t>(0);
    String _activeConfiguration;
    static PinConfig parsePinConfigString(String& pinStr);
    static PinConfig parsePinConfigString(const String& pinStr);
    static void updateLedCb(void* pTimerArg);
    static bool isPinValid(int pin);
    void publishToEventServer();
    void publishToMqtt();
    void publishFinishedStepAnimations();
    void publishColorStayedCmds();
    void checkStableColorState();
    void publishStatus();

    bool clockMaster,
         colorMaster,
         startupColorLast;
    uint32_t clockMasterInterval,
             transFinInterval,
             colorMasterInterval,
             colorMinInterval;

    HSVCT _lastHsvct;
    ChannelOutput _lastOutput;

    StepSync* _stepSync = nullptr;

    uint32_t _stepCounter = 0;
    HSVCT _prevColor;
    uint32_t _numStableColorSteps = 0;
    ChannelOutput _prevOutput;

    static const uint32_t _saveAfterStableColorMs = 2000;

    SimpleTimer _ledTimer;
    uint32_t _timerInterval = RGBWW_MINTIMEDIFF;
    HashMap<String, bool> _stepFinishedAnimations;
    uint32_t _lastColorEvent = 0;
};
