#pragma once

#include <limits>


class StepSync {
public:
    uint32_t onMasterClock(uint32_t stepsCurrent, uint32_t stepsMaster);
    int getCatchupOffset() const;
    uint32_t reset();

protected:
    template<typename T>
    static T calcOverflowVal(T prevValue, T curValue) {
        if (curValue < prevValue) {
            //overflow
            return std::numeric_limits<T>::max() - prevValue + curValue;
        }
        else {
            return curValue - prevValue;
        }
    }

    int _catchupOffset = 0;

private:
    uint32_t _stepsSyncMasterLast = 0;
    uint32_t _stepsSyncLast = 0;
    bool _firstMasterSync = true;
    double _steering = 1.0;
    const uint32_t _constBaseInt = RGBWW_MINTIMEDIFF_US;
};
