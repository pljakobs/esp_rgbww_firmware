#pragma once

#include <RGBWWLed/RGBWWLed.h>
#include <WString.h>
#include <cstdint>
#include <memory>

struct VirtualLight {
    String id;
    String name;
    std::unique_ptr<RGBWWLed> led;
    std::uint16_t index = 0;
    bool isDefault = false;
    ChannelOutput lastEventRaw{};
    HSVCT lastEventHsv{};
    bool lastEventHasHsv = false;
    bool eventStateKnown = false;
    uint32_t lastEventTime = 0;
};