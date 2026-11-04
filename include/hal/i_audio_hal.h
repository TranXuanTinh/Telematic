#pragma once

#include "common/types.h"
#include <cstdint>

namespace tcu::hal {

/// Pure Virtual HAL for Cabin Voice Audio Codec & BIST Loopback Diagnostics.
class IAudioHAL {
public:
    virtual ~IAudioHAL() = default;

    virtual bool initialize() = 0;
    virtual bool setRoute(AudioRoute route) = 0;
    virtual AudioRoute currentRoute() const = 0;

    /// Execute sub-audible/codec loopback test (used by tcu-diagd for UN R144 readiness).
    virtual AudioLoopbackResult runDiagnosticLoopback(uint32_t freq_hz,
                                                      uint32_t duration_ms) = 0;
};

} // namespace tcu::hal

