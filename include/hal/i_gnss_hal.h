#pragma once

#include "common/types.h"
#include <optional>
#include <string>

namespace tcu::hal {

/// Pure Virtual HAL for Multi-Constellation GNSS Receiver & Active Antenna Monitor.
class IGnssHAL {
public:
    virtual ~IGnssHAL() = default;

    virtual bool start(const std::string& device, int rate_hz) = 0;
    virtual bool setUpdateRateHz(int rate_hz) = 0;
    virtual void stop() = 0;

    virtual std::optional<GnssFix> readFix(int timeout_ms) = 0;
};

} // namespace tcu::hal

