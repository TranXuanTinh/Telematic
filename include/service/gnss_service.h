#pragma once

#include "common/types.h"
#include "hal/i_gnss_hal.h"

#include <mutex>

namespace tcu::service {

/// Maintains current WGS84 position + N-1 / N-2 historical breadcrumb trail for eCall MSD & SVT.
class GnssService {
public:
    explicit GnssService(hal::IGnssHAL& hal,
                         double default_lat = 21.028511,
                         double default_lon = 105.804817);

    /// Poll GNSS HAL once and update breadcrumb trail if moved >= 15 meters.
    GnssFix pollAndUpdate(int timeout_ms = 20);

    /// Directly ingest a new fix (useful in deterministic tests).
    void ingestFix(const GnssFix& fix);

    GnssTrail getTrail() const;
    GnssFix getLatestFix() const;
    bool setRateHz(int rate_hz);

private:
    hal::IGnssHAL&     hal_;
    mutable std::mutex mtx_;
    GnssTrail          trail_{};
};

} // namespace tcu::service

