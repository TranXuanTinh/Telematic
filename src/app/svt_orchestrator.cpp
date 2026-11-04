#include "app/svt_orchestrator.h"
#include "common/geo_math.h"
#include "common/json_mini.h"
#include "common/logger.h"

#include <chrono>
#include <cmath>

namespace tcu::app {

SvtOrchestrator::SvtOrchestrator(const TcuConfig& cfg, hal::ICryptoHAL& crypto_hal)
    : cfg_(cfg),
      crypto_hal_(crypto_hal),
      ipc_(ipc::DaemonId::SVTD, cfg.ipc.broker_socket) {
    latest_fix_.lat_deg   = cfg.gnss.default_lat;
    latest_fix_.lon_deg   = cfg.gnss.default_lon;
    latest_fix_.fix_valid = true;
    anchor_lat_           = cfg.gnss.default_lat;
    anchor_lon_           = cfg.gnss.default_lon;
}

SvtOrchestrator::~SvtOrchestrator() {
    stop();
}

std::string SvtOrchestrator::buildSignedCloudCommand(const hal::ICryptoHAL& crypto,
                                                     const std::string& hmac_secret,
                                                     const std::string& action,
                                                     uint64_t nonce) {
    const std::string msg = action + "|" + std::to_string(nonce);
    const std::string mac = crypto.computeHmacSha256Hex(hmac_secret, msg);
    common::JsonValue j = common::JsonValue::object();
    j["action"] = action;
    j["nonce"]  = nonce;
    j["hmac"]   = mac;
    return j.dump(false);
}

bool SvtOrchestrator::start() {
    if (running_.load()) return true;

    registerHandlers();
    if (!ipc_.connect(2000)) {
        return false;
    }

    // Query initial GNSS & Vehicle state
    auto gres = ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::GET_GNSS_TRAIL, "{}", 250);
    if (gres.ok()) {
        if (auto gj = common::JsonValue::parse(gres.payload)) {
            std::lock_guard<std::mutex> lk(mtx_);
            latest_fix_.lat_deg = (*gj)["current"]["lat_deg"].asDouble(cfg_.gnss.default_lat);
            latest_fix_.lon_deg = (*gj)["current"]["lon_deg"].asDouble(cfg_.gnss.default_lon);
            anchor_lat_         = latest_fix_.lat_deg;
            anchor_lon_         = latest_fix_.lon_deg;
        }
    }

    running_.store(true);
    worker_ = std::thread(&SvtOrchestrator::workerLoop, this);
    return true;
}

void SvtOrchestrator::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    ipc_.disconnect();
}

SvtState SvtOrchestrator::currentState() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return state_;
}

std::pair<double, double> SvtOrchestrator::parkedAnchorLatLon() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return {anchor_lat_, anchor_lon_};
}

void SvtOrchestrator::setCustomPolygonGeofence(std::vector<std::pair<double, double>> polygon) {
    std::lock_guard<std::mutex> lk(mtx_);
    polygon_geofence_ = std::move(polygon);
}

void SvtOrchestrator::enterStolenActiveLocked(uint8_t tamper_reason_mask, double breach_dist_m) {
    active_tamper_mask_ |= tamper_reason_mask;
    if (state_ == SvtState::STOLEN_ACTIVE || state_ == SvtState::IMMOBILIZE_PENDING ||
        state_ == SvtState::IMMOBILIZED) {
        return;
    }
    state_ = SvtState::STOLEN_ACTIVE;
    (void)breach_dist_m;
    cv_.notify_all();
}

void SvtOrchestrator::sendCovertTrackingPacket(uint8_t reason_mask, double breach_dist_m) {
    GnssFix fix_snap;
    VehicleState veh_snap;
    SvtState st_snap;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fix_snap = latest_fix_;
        veh_snap = vehicle_;
        st_snap  = state_;
    }

    // Fetch Serving & Neighbor Cell-IDs for triangulation fallback inside containers/garages
    common::JsonValue cell_info = common::JsonValue::object();
    auto mres = ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::GET_MODEM_STATUS, "{}", 150);
    if (mres.ok()) {
        if (auto mj = common::JsonValue::parse(mres.payload)) {
            cell_info = (*mj)["serving_cell"];
        }
    }

    common::JsonValue body = common::JsonValue::object();
    body["vin"]                  = cfg_.system.vin;
    body["svt_state"]            = static_cast<int>(st_snap);
    body["tamper_mask"]          = reason_mask;
    body["breach_distance_m"]    = breach_dist_m;
    body["lat_deg"]              = fix_snap.lat_deg;
    body["lon_deg"]              = fix_snap.lon_deg;
    body["speed_kmh"]            = veh_snap.speed_kmh;
    body["using_backup_battery"] = veh_snap.using_backup_battery;
    body["bub_soc"]              = veh_snap.bub_soc;
    body["serving_cell"]         = cell_info;
    body["stealth_mode"]         = true;
    body["timestamp_utc"]        = currentEpochSeconds();

    common::JsonValue req = common::JsonValue::object();
    req["priority"] = 1; // Priority 1 = SVT_Alarm (preserved over normal telemetry in SQLite)
    req["topic"]    = "tcu/" + cfg_.system.device_id + "/svt_alert";
    req["payload"]  = body.dump(false);

    auto pres = ipc_.callRpc(
        ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::PUBLISH_CLOUD_TELEMETRY, req.dump(false), 300);
    if (pres.ok()) {
        alerts_sent_.fetch_add(1);
    }
}

bool SvtOrchestrator::handleAuthenticatedSocCommand(const std::string& cmd_json) {
    auto j = common::JsonValue::parse(cmd_json);
    if (!j || !j->isObject()) return false;

    const std::string action = (*j)["action"].asString();
    const uint64_t nonce     = (*j)["nonce"].asUint64(0);
    const std::string rx_mac = (*j)["hmac"].asString();

    const std::string expected_mac = crypto_hal_.computeHmacSha256Hex(
        cfg_.cloud.command_hmac_secret, action + "|" + std::to_string(nonce));

    bool trigger_alert = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // Anti-replay check + constant-value HMAC verification
        if (nonce <= last_valid_nonce_ || rx_mac.empty() || rx_mac != expected_mac) {
            return false;
        }
        last_valid_nonce_ = nonce;

        if (action == "SVT_ACTIVATE") {
            enterStolenActiveLocked(TAMPER_REMOTE_DECLARATION);
            trigger_alert = true;
        } else if (action == "SVT_IMMOBILIZE") {
            active_tamper_mask_ |= TAMPER_REMOTE_DECLARATION;
            state_ = SvtState::IMMOBILIZE_PENDING;
            if (vehicle_.speed_kmh == 0.0 && !vehicle_.ignition_kl15) {
                if (!zero_speed_timer_active_) {
                    zero_speed_since_ms_     = steadyNowMs();
                    zero_speed_timer_active_ = true;
                }
            } else {
                zero_speed_timer_active_ = false;
            }
            cv_.notify_all();
        } else if (action == "SVT_DISARM_RECOVERED") {
            state_                            = SvtState::DISARMED;
            active_tamper_mask_               = 0;
            zero_speed_timer_active_          = false;
            suppress_auto_arm_until_activity_ = true;
            cv_.notify_all();
        } else {
            return false;
        }
    }

    if (action == "SVT_DISARM_RECOVERED") {
        common::JsonValue req = common::JsonValue::object();
        req["enable"] = false;
        ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::SET_IMMOBILIZER_STATE, req.dump(false), 250);

        common::JsonValue led = common::JsonValue::object();
        led["led_mode"] = static_cast<int>(EcallLedMode::GREEN_READY);
        ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::SET_ECALL_LED, led.dump(false), 200);

        common::JsonValue gr = common::JsonValue::object();
        gr["rate_hz"] = cfg_.gnss.normal_rate_hz;
        ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::SET_GNSS_RATE, gr.dump(false), 200);

        common::JsonValue ulk = common::JsonValue::object();
        ulk["daemon_id"] = static_cast<int>(ipc::DaemonId::SVTD);
        ulk["reason"]    = "SVT_ALARM";
        ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::RELEASE_POWER_LOCK, ulk.dump(false), 200);
    } else if (trigger_alert) {
        sendCovertTrackingPacket(TAMPER_REMOTE_DECLARATION, 0.0);
    }
    return true;
}

bool SvtOrchestrator::tryEngageZeroSpeedImmobilizer() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (state_ != SvtState::IMMOBILIZE_PENDING) return false;

        // Tier 1 Software Interlock: Speed MUST be 0.0 km/h and KL15 OFF for >= zero_speed_hold_ms
        if (vehicle_.speed_kmh > 0.0 || vehicle_.ignition_kl15) {
            zero_speed_timer_active_ = false;
            return false;
        }
        const uint64_t now = steadyNowMs();
        if (!zero_speed_timer_active_) {
            zero_speed_since_ms_     = now;
            zero_speed_timer_active_ = true;
        }
        if ((now - zero_speed_since_ms_) < static_cast<uint64_t>(cfg_.svt.zero_speed_hold_ms)) {
            return false;
        }
    }

    // Request MCU Hardware Gate to assert BCM Starter Inhibit
    common::JsonValue req = common::JsonValue::object();
    req["enable"] = true;
    auto res = ipc_.callRpc(
        ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::SET_IMMOBILIZER_STATE, req.dump(false), 300);
    if (res.ok()) {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = SvtState::IMMOBILIZED;
        return true;
    }
    return false;
}

void SvtOrchestrator::registerHandlers() {
    ipc_.subscribe(ipc::TopicId::TOPIC_VEHICLE_STATE,
                   [this](ipc::TopicId, const std::string& payload) {
                       auto j = common::JsonValue::parse(payload);
                       if (!j) return;

                       bool newly_stolen = false;
                       {
                           std::lock_guard<std::mutex> lk(mtx_);
                           vehicle_.speed_kmh              = (*j)["speed_kmh"].asDouble();
                           vehicle_.ignition_kl15          = (*j)["ignition_kl15"].asBool();
                           vehicle_.gear                   = static_cast<GearPosition>((*j)["gear"].asInt64(0));
                           vehicle_.doors_locked           = (*j)["doors_locked"].asBool();
                           vehicle_.authorized_driver      = (*j)["authorized_driver"].asBool();
                           vehicle_.kl30_mv                = static_cast<uint16_t>((*j)["kl30_mv"].asUint64(12600));
                           vehicle_.bub_soc                = static_cast<uint8_t>((*j)["bub_soc"].asUint64(95));
                           vehicle_.using_backup_battery   = (*j)["using_backup_battery"].asBool(false);
                           vehicle_.starter_inhibit_active = (*j)["starter_inhibit_active"].asBool(false);

                           if (vehicle_.ignition_kl15 || !vehicle_.doors_locked) {
                               suppress_auto_arm_until_activity_ = false;
                           }

                           if (state_ == SvtState::DISARMED &&
                               !suppress_auto_arm_until_activity_ &&
                               !vehicle_.ignition_kl15 && vehicle_.doors_locked &&
                               vehicle_.speed_kmh == 0.0) {
                               // Auto-arm & lock parked anchor coordinates!
                               state_      = SvtState::ARMED;
                               anchor_lat_ = latest_fix_.lat_deg;
                               anchor_lon_ = latest_fix_.lon_deg;
                           } else if (state_ == SvtState::ARMED &&
                                      vehicle_.authorized_driver && vehicle_.ignition_kl15) {
                               state_ = SvtState::DISARMED;
                           } else if (state_ == SvtState::ARMED &&
                                      vehicle_.kl30_mv < cfg_.svt.kl30_cut_threshold_mv) {
                               enterStolenActiveLocked(TAMPER_KL30_BATTERY_CUT);
                               newly_stolen = true;
                           }

                           if (state_ == SvtState::IMMOBILIZE_PENDING) {
                               if (vehicle_.speed_kmh > 0.0 || vehicle_.ignition_kl15) {
                                   zero_speed_timer_active_ = false;
                               } else if (!zero_speed_timer_active_) {
                                   zero_speed_since_ms_     = steadyNowMs();
                                   zero_speed_timer_active_ = true;
                               }
                           }
                           cv_.notify_all();
                       }

                       if (newly_stolen) {
                           sendCovertTrackingPacket(TAMPER_KL30_BATTERY_CUT, 0.0);
                       }
                   });

    ipc_.subscribe(ipc::TopicId::TOPIC_SECURITY_TAMPER,
                   [this](ipc::TopicId, const std::string& payload) {
                       auto j = common::JsonValue::parse(payload);
                       if (!j) return;
                       const uint8_t flags = static_cast<uint8_t>((*j)["tamper_flags"].asUint64(0));
                       const double pitch  = std::fabs((*j)["tilt_pitch_deg"].asDouble(0.0));
                       const double roll   = std::fabs((*j)["tilt_roll_deg"].asDouble(0.0));

                       bool alarm = false;
                       {
                           std::lock_guard<std::mutex> lk(mtx_);
                           if (state_ == SvtState::ARMED || state_ == SvtState::DISARMED) {
                               if ((flags & TAMPER_KL30_BATTERY_CUT) != 0U ||
                                   ((flags & TAMPER_IMU_TOW_TILT) != 0U &&
                                    (pitch >= cfg_.svt.tilt_threshold_deg ||
                                     roll >= cfg_.svt.tilt_threshold_deg))) {
                                   enterStolenActiveLocked(flags);
                                   alarm = true;
                               }
                           }
                       }
                       if (alarm) {
                           sendCovertTrackingPacket(flags, 0.0);
                       }
                   });

    ipc_.subscribe(ipc::TopicId::TOPIC_GNSS_FIX,
                   [this](ipc::TopicId, const std::string& payload) {
                       auto j = common::JsonValue::parse(payload);
                       if (!j) return;

                       bool geofence_breached = false;
                       double dist_m = 0.0;
                       {
                           std::lock_guard<std::mutex> lk(mtx_);
                           latest_fix_.lat_deg   = (*j)["lat_deg"].asDouble();
                           latest_fix_.lon_deg   = (*j)["lon_deg"].asDouble();
                           latest_fix_.hdop      = static_cast<float>((*j)["hdop"].asDouble(1.0));
                           latest_fix_.fix_valid = (*j)["fix_valid"].asBool(true);

                           if (state_ == SvtState::ARMED && !vehicle_.authorized_driver &&
                               latest_fix_.fix_valid && latest_fix_.hdop <= 2.5f) {
                               if (!polygon_geofence_.empty()) {
                                   if (!common::isPointInsidePolygon(
                                           latest_fix_.lat_deg, latest_fix_.lon_deg, polygon_geofence_)) {
                                       geofence_breached = true;
                                   }
                               } else {
                                   dist_m = common::haversineDistanceMeters(
                                       anchor_lat_, anchor_lon_, latest_fix_.lat_deg, latest_fix_.lon_deg);
                                   if (dist_m > cfg_.svt.geofence_radius_m) {
                                       geofence_breached = true;
                                   }
                               }
                               if (geofence_breached) {
                                   enterStolenActiveLocked(TAMPER_GEOFENCE_BREACH, dist_m);
                               }
                           }
                       }
                       if (geofence_breached) {
                           sendCovertTrackingPacket(TAMPER_GEOFENCE_BREACH, dist_m);
                       }
                   });

    ipc_.subscribe(ipc::TopicId::TOPIC_CLOUD_COMMAND,
                   [this](ipc::TopicId, const std::string& payload) {
                       if (auto ev = common::JsonValue::parse(payload)) {
                           if ((*ev)["mqtt_topic"].asString().find("svt") != std::string::npos) {
                               handleAuthenticatedSocCommand((*ev)["payload"].asString());
                           }
                       }
                   });

    ipc_.subscribe(ipc::TopicId::TOPIC_POWER_TRANSITION,
                   [this](ipc::TopicId, const std::string& payload) {
                       if (auto j = common::JsonValue::parse(payload)) {
                           auto target = static_cast<PowerState>((*j)["target_state"].asInt64(-1));
                           if (target == PowerState::PRE_SLEEP) {
                               {
                                   std::lock_guard<std::mutex> lk(mtx_);
                                   if (state_ == SvtState::DISARMED) {
                                       state_      = SvtState::ARMED;
                                       anchor_lat_ = latest_fix_.lat_deg;
                                       anchor_lon_ = latest_fix_.lon_deg;
                                   }
                               }
                               common::JsonValue ack = common::JsonValue::object();
                               ack["daemon_id"] = static_cast<int>(ipc::DaemonId::SVTD);
                               ipc_.callRpc(ipc::DaemonId::MCU_IPCD,
                                            ipc::RpcMethodId::SLEEP_READY_ACK,
                                            ack.dump(false),
                                            300);
                           }
                       }
                   });

    ipc_.registerRpc(ipc::RpcMethodId::INJECT_TEST_EVENT,
                     [this](const std::string& req) -> ipc::RpcResult {
                         bool ok = handleAuthenticatedSocCommand(req);
                         return {ok ? ipc::IpcStatus::OK : ipc::IpcStatus::ERR_REJECTED,
                                 ok ? "{\"ok\":true}" : "{\"ok\":false}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::GET_DAEMON_STATUS,
                     [this](const std::string&) -> ipc::RpcResult {
                         auto [alat, alon] = parkedAnchorLatLon();
                         common::JsonValue j = common::JsonValue::object();
                         j["daemon"]       = "tcu-svtd";
                         j["state"]        = static_cast<int>(currentState());
                         j["alerts_sent"]  = publishedAlertCount();
                         j["anchor_lat"]   = alat;
                         j["anchor_lon"]   = alon;
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });
}

void SvtOrchestrator::workerLoop() {
    uint64_t last_report_ms = 0;
    bool stealth_configured = false;

    while (running_.load()) {
        SvtState st;
        double speed_kmh = 0.0;
        uint8_t mask     = 0;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(40));
            if (!running_.load()) break;
            st        = state_;
            speed_kmh = vehicle_.speed_kmh;
            mask      = active_tamper_mask_;
        }

        if (st == SvtState::STOLEN_ACTIVE || st == SvtState::IMMOBILIZE_PENDING ||
            st == SvtState::IMMOBILIZED) {
            if (!stealth_configured) {
                stealth_configured = true;
                // Stealth Mode: turn cabin LED OFF, acquire WakeLock, set GNSS 10Hz
                common::JsonValue wlk = common::JsonValue::object();
                wlk["daemon_id"] = static_cast<int>(ipc::DaemonId::SVTD);
                wlk["reason"]    = "SVT_ALARM";
                ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::REQUEST_POWER_LOCK, wlk.dump(false), 200);

                common::JsonValue led = common::JsonValue::object();
                led["led_mode"] = static_cast<int>(EcallLedMode::OFF);
                ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::SET_ECALL_LED, led.dump(false), 200);

                common::JsonValue gr = common::JsonValue::object();
                gr["rate_hz"] = cfg_.gnss.emergency_rate_hz;
                ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::SET_GNSS_RATE, gr.dump(false), 200);
            }

            if (st == SvtState::IMMOBILIZE_PENDING) {
                tryEngageZeroSpeedImmobilizer();
            }

            const uint64_t now = steadyNowMs();
            const uint64_t period = (speed_kmh > 0.5)
                ? static_cast<uint64_t>(cfg_.svt.moving_report_interval_ms)
                : static_cast<uint64_t>(cfg_.svt.stationary_report_interval_ms);

            if (now - last_report_ms >= period) {
                last_report_ms = now;
                sendCovertTrackingPacket(mask, 0.0);
            }
        } else {
            stealth_configured = false;
        }
    }
}

} // namespace tcu::app
