#pragma once

#include <RGBWWLed/RGBWWLed.h>
#include <WString.h>
#include <memory>

struct VirtualLight {
    String id;
    String name;
    std::unique_ptr<RGBWWLed> led;
    uint16_t index = 0;
    bool isDefault = false;
};