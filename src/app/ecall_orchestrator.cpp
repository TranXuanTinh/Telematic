#include "app/ecall_orchestrator.h"
#include "common/json_mini.h"
#include "common/logger.h"

#include <chrono>
#include <pthread.h>
#include <sched.h>

namespace tcu::app {

namespace {
GnssFix parseFixJson(const common::JsonValue& j) {
    GnssFix f;
    f.lat_deg            = j["lat_deg"].asDouble(21.028511);
    f.lon_deg            = j["lon_deg"].asDouble(105.804817);
    f.alt_m              = j["alt_m"].asDouble(12.0);
    f.heading_deg        = static_cast<float>(j["heading_deg"].asDouble(0.0));
    f.speed_mps          = static_cast<float>(j["speed_mps"].asDouble(0.0));
    f.hdop               = static_cast<float>(j["hdop"].asDouble(1.0));
    f.satellites         = static_cast<uint8_t>(j["satellites"].asUint64(10));
    f.fix_valid          = j["fix_valid"].asBool(true);
    f.antenna_health     = static_cast<AntennaHealth>(j["antenna_health"].asUint64(0));
    f.antenna_current_ma = static_cast<float>(j["antenna_current_ma"].asDouble(18.0));
    f.timestamp_utc      = j["timestamp_utc"].asUint64(currentEpochSeconds());
    return f;
}
} // namespace

EcallOrchestrator::EcallOrchestrator(const TcuConfig& cfg)
    : cfg_(cfg), ipc_(ipc::DaemonId::ECALLD, cfg.ipc.broker_socket) {}

EcallOrchestrator::~EcallOrchestrator() {
    stop();
}

bool EcallOrchestrator::start() {
    if (running_.load()) return true;

    registerHandlers();
    if (!ipc_.connect(2000)) {
        return false;
    }

    running_.store(true);
    fsm_thread_ = std::thread(&EcallOrchestrator::fsmWorkerLoop, this);

    // Attempt POSIX real-time SCHED_FIFO priority 80 per Threading Model Section 2
    sched_param sp{};
    sp.sched_priority = 80;
    (void)pthread_setschedparam(fsm_thread_.native_handle(), SCHED_FIFO, &sp);

    return true;
}

void EcallOrchestrator::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (fsm_thread_.joinable()) {
        fsm_thread_.join();
    }
    ipc_.disconnect();
}

EcallState EcallOrchestrator::currentState() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return state_;
}

std::optional<common::EcallMsdPayload> EcallOrchestrator::lastDecodedMsd() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_msd_;
}

void EcallOrchestrator::onSosButtonPressed() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (state_ != EcallState::IDLE_ARMED) return;
    sos_held_           = true;
    sos_press_start_ms_ = steadyNowMs();
    state_              = EcallState::SOS_DEBOUNCE;
    cv_.notify_all();
}

void EcallOrchestrator::onSosButtonReleased() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!sos_held_) return;
    sos_held_ = false;
    if (state_ == EcallState::SOS_DEBOUNCE) {
        const uint64_t held_ms = steadyNowMs() - sos_press_start_ms_;
        if (held_ms < static_cast<uint64_t>(cfg_.ecall.sos_debounce_ms)) {
            // Short accidental press < 2.0s: cancel cleanly back to IDLE_ARMED
            state_ = EcallState::IDLE_ARMED;
        }
    }
    cv_.notify_all();
}

void EcallOrchestrator::triggerEmergency(const CrashEvent& crash) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (state_ != EcallState::IDLE_ARMED && state_ != EcallState::SOS_DEBOUNCE &&
        state_ != EcallState::CALLBACK_STANDBY) {
        return; // Already executing an active emergency call
    }
    sos_held_      = false;
    pending_crash_ = crash;
    cv_.notify_all();
}

GnssTrail EcallOrchestrator::fetchGnssTrailViaIpc() {
    GnssTrail trail;
    trail.current.lat_deg = cfg_.gnss.default_lat;
    trail.current.lon_deg = cfg_.gnss.default_lon;
    trail.current.fix_valid = true;

    auto res = ipc_.callRpc(
        ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::GET_GNSS_TRAIL, "{}", 80, true);
    if (res.ok()) {
        if (auto j = common::JsonValue::parse(res.payload)) {
            trail.current = parseFixJson((*j)["current"]);
            trail.has_n1  = (*j)["has_n1"].asBool(false);
            if (trail.has_n1) trail.n1 = parseFixJson((*j)["n1"]);
            trail.has_n2  = (*j)["has_n2"].asBool(false);
            if (trail.has_n2) trail.n2 = parseFixJson((*j)["n2"]);
        }
    }
    return trail;
}

bool EcallOrchestrator::retransmitMsd() {
    CrashEvent crash_copy;
    uint8_t next_id = 1;
    std::string dial_num;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (state_ != EcallState::VOICE_ACTIVE) return false;
        state_     = EcallState::TRANSMITTING_MSD;
        crash_copy = active_crash_;
        next_id    = ++msd_seq_id_;
        dial_num   = crash_copy.test_call ? cfg_.modem.test_psap_number : cfg_.modem.emergency_number;
    }

    auto trail = fetchGnssTrailViaIpc();
    auto msd_payload = common::MsdCodec::buildFromTelemetry(
        cfg_.system.vin,
        cfg_.ecall.vehicle_type,
        cfg_.ecall.propulsion_mask,
        next_id,
        crash_copy,
        trail);
    auto msd_raw = common::MsdCodec::encode(msd_payload);

    common::JsonValue req = common::JsonValue::object();
    req["dial_number"] = dial_num;
    req["test_call"]   = crash_copy.test_call;
    req["msd_hex"]     = common::bytesToHex(msd_raw.data(), msd_raw.size());
    req["timeout_ms"]  = cfg_.ecall.msd_tx_timeout_ms;

    auto res = ipc_.callRpc(
        ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::DIAL_EMERGENCY_AND_MSD, req.dump(false), 1500, true);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        last_msd_ = msd_payload;
        state_    = EcallState::VOICE_ACTIVE;
    }
    common::JsonValue aud = common::JsonValue::object();
    aud["route"] = static_cast<int>(AudioRoute::MODEM_VOICE_TO_CABIN);
    ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::SET_VOICE_AUDIO_ROUTE, aud.dump(false), 150, true);
    return res.ok();
}

void EcallOrchestrator::onPsapOperatorHangup() {
    bool is_test = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (state_ != EcallState::VOICE_ACTIVE) return;
        is_test = active_crash_.test_call;
    }

    ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::HANGUP_CALL, "{}", 200, true);

    if (is_test) {
        // Non-emergency test calls finish immediately without locking 60-minute PSAP standby
        endCallbackStandbyWindow();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = EcallState::CALLBACK_STANDBY;
        callback_deadline_ms_ =
            steadyNowMs() + static_cast<uint64_t>(cfg_.ecall.callback_standby_sec) * 1000ULL;
    }
    cv_.notify_all();
}

void EcallOrchestrator::endCallbackStandbyWindow() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = EcallState::IDLE_ARMED;
        callback_deadline_ms_ = 0;
    }

    ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::RELEASE_EMERGENCY_CHANNEL, "{}", 200, true);

    common::JsonValue led = common::JsonValue::object();
    led["led_mode"] = static_cast<int>(EcallLedMode::GREEN_READY);
    ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::SET_ECALL_LED, led.dump(false), 200);

    common::JsonValue ulk = common::JsonValue::object();
    ulk["daemon_id"] = static_cast<int>(ipc::DaemonId::ECALLD);
    ulk["reason"]    = "EMERGENCY";
    ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::RELEASE_POWER_LOCK, ulk.dump(false), 200);
}

void EcallOrchestrator::executeEmergencySession(const CrashEvent& crash) {
    const uint64_t t0_ms = steadyNowMs();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_        = EcallState::PREEMPT_AND_LOCK;
        active_crash_ = crash;
        msd_seq_id_   = 1;
    }

    // 1. Acquire unconditional WakeLock & preempt Modem/Audio/OTA
    common::JsonValue wlk = common::JsonValue::object();
    wlk["daemon_id"] = static_cast<int>(ipc::DaemonId::ECALLD);
    wlk["reason"]    = "EMERGENCY";
    ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::REQUEST_POWER_LOCK, wlk.dump(false), 80, true);

    ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::ACQUIRE_EMERGENCY_CHANNEL, "{}", 80, true);

    common::JsonValue led = common::JsonValue::object();
    led["led_mode"] = static_cast<int>(EcallLedMode::BLINK_CALL);
    ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::SET_ECALL_LED, led.dump(false), 80, true);

    // 2. Build 140-byte CEN EN 15722 MSD
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = EcallState::BUILD_MSD;
    }
    GnssTrail trail = fetchGnssTrailViaIpc();
    auto msd_payload = common::MsdCodec::buildFromTelemetry(
        cfg_.system.vin,
        cfg_.ecall.vehicle_type,
        cfg_.ecall.propulsion_mask,
        1,
        crash,
        trail);
    auto msd_bytes = common::MsdCodec::encode(msd_payload);

    // 3. Dial 112 (or Test PSAP) and transmit MSD with retry backoff
    const std::string target_num =
        crash.test_call ? cfg_.modem.test_psap_number : cfg_.modem.emergency_number;

    bool call_established = false;
    for (int attempt = 0; attempt < std::max(1, cfg_.ecall.max_dial_retries) && running_.load();
         ++attempt) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = EcallState::DIALING_PSAP;
        }

        common::JsonValue req = common::JsonValue::object();
        req["dial_number"] = target_num;
        req["test_call"]   = crash.test_call;
        req["msd_hex"]     = common::bytesToHex(msd_bytes.data(), msd_bytes.size());
        req["timeout_ms"]  = cfg_.ecall.msd_tx_timeout_ms;

        if (attempt == 0) {
            last_dial_latency_ms_.store(steadyNowMs() - t0_ms);
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = EcallState::TRANSMITTING_MSD;
        }
        auto dial_res = ipc_.callRpc(
            ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::DIAL_EMERGENCY_AND_MSD, req.dump(false), 1500, true);

        if (dial_res.ok()) {
            call_established = true;
            break;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = EcallState::RETRY_BACKOFF;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!call_established) {
        endCallbackStandbyWindow();
        return;
    }

    // 4. Open Bidirectional Cabin Voice Audio (even if MSD ACK timed out, per EN 16072 fallback!)
    common::JsonValue aud = common::JsonValue::object();
    aud["route"] = static_cast<int>(AudioRoute::MODEM_VOICE_TO_CABIN);
    ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::SET_VOICE_AUDIO_ROUTE, aud.dump(false), 150, true);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        last_msd_ = msd_payload;
        state_    = EcallState::VOICE_ACTIVE;
    }
    completed_calls_.fetch_add(1);

    // Log priority-0 emergency event to Cloud / SQLite audit queue
    common::JsonValue audit = common::JsonValue::object();
    audit["priority"] = 0;
    audit["topic"]    = "tcu/" + cfg_.system.device_id + "/ecall";
    common::JsonValue body = common::JsonValue::object();
    body["vin"]        = cfg_.system.vin;
    body["auto"]       = crash.automatic_activation;
    body["test_call"]  = crash.test_call;
    body["lat_deg"]    = trail.current.lat_deg;
    body["lon_deg"]    = trail.current.lon_deg;
    body["delta_v"]    = crash.delta_v_x_kmh;
    audit["payload"]   = body.dump(false);
    ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::PUBLISH_CLOUD_TELEMETRY, audit.dump(false), 300);
}

void EcallOrchestrator::registerHandlers() {
    ipc_.subscribe(ipc::TopicId::TOPIC_EMERGENCY_TRIGGER,
                   [this](ipc::TopicId, const std::string& payload) {
                       if (auto j = common::JsonValue::parse(payload)) {
                           CrashEvent c;
                           c.automatic_activation = (*j)["automatic_activation"].asBool(true);
                           c.test_call            = (*j)["test_call"].asBool(false);
                           c.airbag_mask          = static_cast<uint8_t>((*j)["airbag_mask"].asUint64(1));
                           c.delta_v_x_kmh        = static_cast<int16_t>((*j)["delta_v_x_kmh"].asInt64(-48));
                           c.delta_v_y_kmh        = static_cast<int16_t>((*j)["delta_v_y_kmh"].asInt64(0));
                           c.rollover             = (*j)["rollover"].asBool(false);
                           c.timestamp_utc        = (*j)["timestamp_utc"].asUint64(currentEpochSeconds());
                           triggerEmergency(c);
                       }
                   });

    ipc_.subscribe(ipc::TopicId::TOPIC_INCOMING_CALL,
                   [this](ipc::TopicId, const std::string&) {
                       bool should_auto_answer = false;
                       {
                           std::lock_guard<std::mutex> lk(mtx_);
                           if (state_ == EcallState::CALLBACK_STANDBY) {
                               state_             = EcallState::VOICE_ACTIVE;
                               should_auto_answer = true;
                           }
                       }
                       if (should_auto_answer) {
                           ipc_.callRpc(ipc::DaemonId::PLATFORMD,
                                        ipc::RpcMethodId::ANSWER_INCOMING_CALL,
                                        "{}",
                                        300,
                                        true);
                       }
                   });

    ipc_.registerRpc(ipc::RpcMethodId::INJECT_TEST_EVENT,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             std::string cmd = (*j)["cmd"].asString();
                             if (cmd == "sos_press") {
                                 onSosButtonPressed();
                             } else if (cmd == "sos_release") {
                                 onSosButtonReleased();
                             } else if (cmd == "psap_hangup") {
                                 onPsapOperatorHangup();
                             } else if (cmd == "test_ecall") {
                                 CrashEvent c;
                                 c.automatic_activation = false;
                                 c.test_call            = true;
                                 c.delta_v_x_kmh        = 0;
                                 c.timestamp_utc        = currentEpochSeconds();
                                 triggerEmergency(c);
                             }
                         }
                         return {ipc::IpcStatus::OK, "{\"ok\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::GET_DAEMON_STATUS,
                     [this](const std::string&) -> ipc::RpcResult {
                         common::JsonValue j = common::JsonValue::object();
                         j["daemon"]           = "tcu-ecalld";
                         j["state"]            = static_cast<int>(currentState());
                         j["completed_calls"]  = completedEcallCount();
                         j["dial_latency_ms"]  = lastTriggerToDialLatencyMs();
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });
}

void EcallOrchestrator::fsmWorkerLoop() {
    while (running_.load()) {
        std::optional<CrashEvent> crash_to_run;
        bool callback_expired = false;

        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(25), [&] {
                return !running_.load() || pending_crash_.has_value();
            });
            if (!running_.load()) break;

            const uint64_t now = steadyNowMs();

            // Check manual SOS button hold >= sos_debounce_ms (2.0s)
            if (state_ == EcallState::SOS_DEBOUNCE && sos_held_ &&
                (now - sos_press_start_ms_) >= static_cast<uint64_t>(cfg_.ecall.sos_debounce_ms)) {
                sos_held_ = false;
                CrashEvent manual{};
                manual.automatic_activation = false;
                manual.test_call            = false;
                manual.airbag_mask          = 0;
                manual.delta_v_x_kmh        = 0;
                manual.timestamp_utc        = currentEpochSeconds();
                pending_crash_              = manual;
            }

            if (pending_crash_.has_value()) {
                crash_to_run = *pending_crash_;
                pending_crash_.reset();
            } else if (state_ == EcallState::CALLBACK_STANDBY &&
                       callback_deadline_ms_ > 0 && now >= callback_deadline_ms_) {
                callback_expired = true;
            }
        }

        if (crash_to_run.has_value()) {
            executeEmergencySession(*crash_to_run);
        } else if (callback_expired) {
            endCallbackStandbyWindow();
        }
    }
}

} // namespace tcu::app

