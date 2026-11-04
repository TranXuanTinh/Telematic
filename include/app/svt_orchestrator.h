#pragma once

#include "common/types.h"
#include "hal/i_crypto_hal.h"
#include "ipc/ipc_client.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tcu::app {

/// Tier 1 Feature Orchestrator for tcu-svtd:
/// Implements Parked Geofencing, IMU Tow/Tilt & KL30 Wire-Cut BUB Failover Detection,
/// Stealth GNSS + Cell-ID Tracking, and Dual-Gated Zero-Speed Immobilizer Inhibit.
class SvtOrchestrator {
public:
    SvtOrchestrator(const TcuConfig& cfg, hal::ICryptoHAL& crypto_hal);
    ~SvtOrchestrator();

    bool start();
    void stop();

    SvtState currentState() const;
    uint32_t publishedAlertCount() const noexcept { return alerts_sent_.load(); }
    std::pair<double, double> parkedAnchorLatLon() const;

    /// Optional custom polygonal geofence; when empty, circular radius geofence_radius_m is used.
    void setCustomPolygonGeofence(std::vector<std::pair<double, double>> polygon);

    /// Helper to build an HMAC-SHA256 authenticated Cloud SOC command JSON.
    static std::string buildSignedCloudCommand(const hal::ICryptoHAL& crypto,
                                               const std::string& hmac_secret,
                                               const std::string& action,
                                               uint64_t nonce);

    bool handleAuthenticatedSocCommand(const std::string& cmd_json);

private:
    void registerHandlers();
    void workerLoop();
    void enterStolenActiveLocked(uint8_t tamper_reason_mask, double breach_dist_m = 0.0);
    void sendCovertTrackingPacket(uint8_t reason_mask, double breach_dist_m);
    bool tryEngageZeroSpeedImmobilizer();

    TcuConfig                               cfg_;
    hal::ICryptoHAL&                        crypto_hal_;
    ipc::IpcClient                          ipc_;

    std::atomic<bool>                       running_{false};
    std::atomic<uint32_t>                   alerts_sent_{0};
    std::thread                             worker_;

    mutable std::mutex                      mtx_;
    std::condition_variable                 cv_;
    SvtState                                state_{SvtState::DISARMED};
    VehicleState                            vehicle_{};
    GnssFix                                 latest_fix_{};
    double                                  anchor_lat_{0.0};
    double                                  anchor_lon_{0.0};
    std::vector<std::pair<double, double>>  polygon_geofence_;
    uint8_t                                 active_tamper_mask_{0};
    uint64_t                                last_valid_nonce_{0};
    uint64_t                                zero_speed_since_ms_{0};
    bool                                    zero_speed_timer_active_{false};
    bool                                    suppress_auto_arm_until_activity_{false};
};

} // namespace tcu::app
