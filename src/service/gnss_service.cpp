#include "service/gnss_service.h"
#include "common/geo_math.h"

namespace tcu::service {

GnssService::GnssService(hal::IGnssHAL& hal, double default_lat, double default_lon)
    : hal_(hal) {
    trail_.current.lat_deg            = default_lat;
    trail_.current.lon_deg            = default_lon;
    trail_.current.fix_valid          = true;
    trail_.current.antenna_health     = AntennaHealth::OK;
    trail_.current.antenna_current_ma = 18.0f;
    trail_.current.timestamp_utc      = currentEpochSeconds();
}

GnssFix GnssService::pollAndUpdate(int timeout_ms) {
    auto fix_opt = hal_.readFix(timeout_ms);
    if (fix_opt.has_value()) {
        ingestFix(*fix_opt);
    }
    return getLatestFix();
}

void GnssService::ingestFix(const GnssFix& fix) {
    std::lock_guard<std::mutex> lk(mtx_);
    trail_.current.antenna_health     = fix.antenna_health;
    trail_.current.antenna_current_ma = fix.antenna_current_ma;

    if (!fix.fix_valid) {
        // Dead-reckoning hold: keep last valid lat/lon coordinates for emergency fallback,
        // while marking current fix validity accurately
        trail_.current.fix_valid     = false;
        trail_.current.timestamp_utc = fix.timestamp_utc;
        return;
    }

    const double dist = common::haversineDistanceMeters(
        trail_.current.lat_deg, trail_.current.lon_deg, fix.lat_deg, fix.lon_deg);

    if (dist >= 15.0) {
        if (trail_.has_n1) {
            trail_.n2     = trail_.n1;
            trail_.has_n2 = true;
        }
        trail_.n1     = trail_.current;
        trail_.has_n1 = true;
    }
    trail_.current = fix;
}

GnssTrail GnssService::getTrail() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return trail_;
}

GnssFix GnssService::getLatestFix() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return trail_.current;
}

bool GnssService::setRateHz(int rate_hz) {
    return hal_.setUpdateRateHz(rate_hz);
}

} // namespace tcu::service

