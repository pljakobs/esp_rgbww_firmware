#pragma once

#include <RGBWWLed/RGBWWLed.h>
#include <WString.h>
#include <memory>

struct VirtualLight {
    String id;
    String name;
    std::unique_ptr<RGBWWLed> led;
};