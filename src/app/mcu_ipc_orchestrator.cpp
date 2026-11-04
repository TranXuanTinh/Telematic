#include "app/mcu_ipc_orchestrator.h"
#include "common/json_mini.h"
#include "common/logger.h"

#include <chrono>
#include <cstring>

namespace tcu::app {

namespace {
common::JsonValue serializeVehicleJson(const VehicleState& v) {
    common::JsonValue j = common::JsonValue::object();
    j["speed_kmh"]              = v.speed_kmh;
    j["ignition_kl15"]          = v.ignition_kl15;
    j["gear"]                   = static_cast<int>(v.gear);
    j["doors_locked"]           = v.doors_locked;
    j["authorized_driver"]      = v.authorized_driver;
    j["kl30_mv"]                = v.kl30_mv;
    j["bub_mv"]                 = v.bub_mv;
    j["bub_soc"]                = v.bub_soc;
    j["bub_soh"]                = v.bub_soh;
    j["using_backup_battery"]   = v.using_backup_battery;
    j["starter_inhibit_active"] = v.starter_inhibit_active;
    j["timestamp_utc"]          = v.timestamp_utc;
    return j;
}
} // namespace

McuIpcOrchestrator::McuIpcOrchestrator(const TcuConfig& cfg,
                                       hal::IMcuIpcHAL& mcu_hal,
                                       hal::IWatchdogHAL& watchdog_hal)
    : cfg_(cfg),
      mcu_hal_(mcu_hal),
      watchdog_hal_(watchdog_hal),
      broker_(cfg.ipc.broker_socket),
      ipc_(ipc::DaemonId::MCU_IPCD, cfg.ipc.broker_socket),
      mcu_proto_(mcu_hal, cfg.mcu_link.ack_timeout_ms, cfg.mcu_link.max_retries) {}

McuIpcOrchestrator::~McuIpcOrchestrator() {
    stop();
}

bool McuIpcOrchestrator::start() {
    if (running_.load()) return true;

    if (!broker_.start()) {
        common::Logger::error("Failed to start UdsBroker on " + cfg_.ipc.broker_socket);
        return false;
    }

    registerRpcHandlers();
    if (!ipc_.connect(2000)) {
        broker_.stop();
        return false;
    }

    hal::McuLinkOpenParams open_params;
    open_params.use_mock_socket  = (cfg_.system.hal_mode == "mock");
    open_params.device_or_socket = open_params.use_mock_socket
                                       ? cfg_.mcu_link.mock_socket
                                       : cfg_.mcu_link.spi_device;
    open_params.speed_hz_or_baud = cfg_.mcu_link.spi_speed_hz;

    if (!mcu_hal_.open(open_params)) {
        common::Logger::warn("Companion MCU link not yet open: " + open_params.device_or_socket);
    }

    mcu_proto_.setFrameCallback([this](const service::McuWireFrame& f) { onMcuFrame(f); });
    mcu_proto_.start();

    watchdog_hal_.open(cfg_.diagnostics.watchdog_device, cfg_.diagnostics.watchdog_timeout_sec);
    watchdog_hal_.notifySystemdReady();

    running_.store(true);
    hb_thread_ = std::thread(&McuIpcOrchestrator::heartbeatLoop, this);
    return true;
}

void McuIpcOrchestrator::stop() {
    if (!running_.exchange(false)) return;
    if (hb_thread_.joinable()) {
        hb_thread_.join();
    }
    watchdog_hal_.closeGracefully();
    mcu_proto_.stop();
    mcu_hal_.close();
    ipc_.disconnect();
    broker_.stop();
}

VehicleState McuIpcOrchestrator::latestVehicleState() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return vehicle_;
}

EcallLedMode McuIpcOrchestrator::currentLedMode() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return led_mode_;
}

void McuIpcOrchestrator::onMcuFrame(const service::McuWireFrame& frame) {
    switch (frame.msg_id) {
        case service::McuMsgId::MSG_WAKEUP_REASON: {
            if (!frame.payload.empty()) {
                auto reason = static_cast<WakeupReason>(frame.payload[0]);
                power_mgr_.setPowerState(PowerState::ACTIVE_RUN, reason);
                common::JsonValue j = common::JsonValue::object();
                j["target_state"]  = static_cast<int>(PowerState::ACTIVE_RUN);
                j["wakeup_reason"] = static_cast<int>(reason);
                ipc_.publish(ipc::TopicId::TOPIC_POWER_TRANSITION, j.dump(false));
            }
            break;
        }

        case service::McuMsgId::MSG_POWER_TRANSITION: {
            if (!frame.payload.empty()) {
                auto target = static_cast<PowerState>(frame.payload[0]);
                if (target == PowerState::PRE_SLEEP && !power_mgr_.hasActiveWakeLocks()) {
                    power_mgr_.beginSleepPreparation();
                    common::JsonValue j = common::JsonValue::object();
                    j["target_state"] = static_cast<int>(PowerState::PRE_SLEEP);
                    ipc_.publish(ipc::TopicId::TOPIC_POWER_TRANSITION, j.dump(false));
                }
            }
            break;
        }

        case service::McuMsgId::MSG_VEHICLE_TELEMETRY: {
            if (frame.payload.size() >= sizeof(service::PackedVehicleTelemetry)) {
                service::PackedVehicleTelemetry pt{};
                std::memcpy(&pt, frame.payload.data(), sizeof(pt));

                VehicleState v;
                v.speed_kmh              = static_cast<double>(pt.speed_cms) / 100.0;
                v.ignition_kl15          = (pt.ignition_kl15 != 0U);
                v.gear                   = static_cast<GearPosition>(pt.gear);
                v.doors_locked           = (pt.doors_locked != 0U);
                v.authorized_driver      = (pt.authorized_driver != 0U);
                v.kl30_mv                = pt.kl30_mv;
                v.bub_mv                 = pt.bub_mv;
                v.bub_soc                = pt.bub_soc;
                v.bub_soh                = pt.bub_soh;
                v.using_backup_battery   = (pt.kl30_mv < cfg_.svt.kl30_cut_threshold_mv);
                v.starter_inhibit_active = (pt.starter_inhibit != 0U);
                v.timestamp_utc          = currentEpochSeconds();

                {
                    std::lock_guard<std::mutex> lk(state_mtx_);
                    vehicle_ = v;
                }
                ipc_.publish(ipc::TopicId::TOPIC_VEHICLE_STATE, serializeVehicleJson(v).dump(false));
            }
            break;
        }

        case service::McuMsgId::MSG_CRASH_EVENT: {
            if (frame.payload.size() >= sizeof(service::PackedCrashEvent)) {
                service::PackedCrashEvent pc{};
                std::memcpy(&pc, frame.payload.data(), sizeof(pc));

                common::JsonValue j = common::JsonValue::object();
                j["automatic_activation"] = (pc.automatic_activation != 0U);
                j["test_call"]            = false;
                j["airbag_mask"]          = pc.airbag_mask;
                j["delta_v_x_kmh"]        = pc.dv_x_kmh;
                j["delta_v_y_kmh"]        = pc.dv_y_kmh;
                j["rollover"]             = (pc.rollover != 0U);
                j["timestamp_utc"]        = currentEpochSeconds();

                // Urgent P0 broadcast to tcu-ecalld, tcu-otad, and tcu-platformd!
                ipc_.publish(ipc::TopicId::TOPIC_EMERGENCY_TRIGGER, j.dump(false), true);
            }
            break;
        }

        case service::McuMsgId::MSG_TAMPER_EVENT: {
            if (frame.payload.size() >= sizeof(service::PackedTamperEvent)) {
                service::PackedTamperEvent te{};
                std::memcpy(&te, frame.payload.data(), sizeof(te));

                common::JsonValue j = common::JsonValue::object();
                j["tamper_flags"]   = te.tamper_flags;
                j["tilt_pitch_deg"] = static_cast<double>(te.tilt_pitch_cdeg) / 100.0;
                j["tilt_roll_deg"]  = static_cast<double>(te.tilt_roll_cdeg) / 100.0;
                j["timestamp_utc"]  = currentEpochSeconds();

                ipc_.publish(ipc::TopicId::TOPIC_SECURITY_TAMPER, j.dump(false), true);
            }
            break;
        }

        case service::McuMsgId::MSG_UDS_ISOTP_PDU: {
            // Bridge ISO-TP UDS request from CAN -> tcu-diagd -> back to MCU CAN!
            if (frame.payload.size() > 2) {
                std::string hex_req = common::bytesToHex(
                    frame.payload.data() + 2, frame.payload.size() - 2);
                common::JsonValue req_j = common::JsonValue::object();
                req_j["pdu_hex"] = hex_req;
                auto res = ipc_.callRpc(
                    ipc::DaemonId::DIAGD, ipc::RpcMethodId::PROCESS_UDS_PDU, req_j.dump(false), 200);
                if (res.ok()) {
                    if (auto rj = common::JsonValue::parse(res.payload)) {
                        auto resp_bytes = common::hexToBytes((*rj)["pdu_hex"].asString());
                        std::vector<uint8_t> mcu_pay = {0x6F, 0x07}; // TX CAN ID 0x76F LE
                        mcu_pay.insert(mcu_pay.end(), resp_bytes.begin(), resp_bytes.end());
                        mcu_proto_.sendUnreliable(service::McuMsgId::MSG_UDS_ISOTP_PDU, mcu_pay);
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

void McuIpcOrchestrator::registerRpcHandlers() {
    ipc_.registerRpc(ipc::RpcMethodId::REQUEST_POWER_LOCK,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             auto holder = static_cast<ipc::DaemonId>((*j)["daemon_id"].asUint64(0));
                             power_mgr_.acquireWakeLock(holder, (*j)["reason"].asString("lock"));
                         }
                         return {ipc::IpcStatus::OK, "{\"locked\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::RELEASE_POWER_LOCK,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             auto holder = static_cast<ipc::DaemonId>((*j)["daemon_id"].asUint64(0));
                             power_mgr_.releaseWakeLock(holder, (*j)["reason"].asString(""));
                         }
                         return {ipc::IpcStatus::OK, "{\"released\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::GET_VEHICLE_STATE,
                     [this](const std::string&) -> ipc::RpcResult {
                         return {ipc::IpcStatus::OK,
                                 serializeVehicleJson(latestVehicleState()).dump(false)};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::FLASH_MCU_FIRMWARE_BANK,
                     [this](const std::string& req) -> ipc::RpcResult {
                         auto j = common::JsonValue::parse(req);
                         if (!j) return {ipc::IpcStatus::ERR_REJECTED, "{\"ok\":false}"};
                         auto fw_bytes = common::hexToBytes((*j)["fw_hex"].asString());
                         uint32_t ver_code = static_cast<uint32_t>((*j)["fw_version_code"].asUint64(1));
                         bool ok = mcu_proto_.flashMcuFirmware(fw_bytes, ver_code);
                         return {ok ? ipc::IpcStatus::OK : ipc::IpcStatus::ERR_REJECTED,
                                 ok ? "{\"ok\":true}" : "{\"ok\":false}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::SET_IMMOBILIZER_STATE,
                     [this](const std::string& req) -> ipc::RpcResult {
                         auto j = common::JsonValue::parse(req);
                         if (!j) return {ipc::IpcStatus::ERR_REJECTED, "{\"ok\":false}"};
                         bool enable = (*j)["enable"].asBool(false);
                         std::vector<uint8_t> pay = {enable ? uint8_t{1} : uint8_t{0},
                                                     0xEF, 0xBE, 0xAD, 0xDE};
                         std::vector<uint8_t> ack;
                         bool sent = mcu_proto_.sendReliable(
                             service::McuMsgId::MSG_SET_IMMOBILIZER, pay, &ack, 250);
                         bool engaged = sent && !ack.empty() && ack[0] == 1U;
                         return {engaged ? ipc::IpcStatus::OK : ipc::IpcStatus::ERR_REJECTED,
                                 engaged ? "{\"ok\":true}" : "{\"ok\":false}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::SET_ECALL_LED,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             auto mode = static_cast<EcallLedMode>((*j)["led_mode"].asUint64(1));
                             {
                                 std::lock_guard<std::mutex> lk(state_mtx_);
                                 led_mode_ = mode;
                             }
                             mcu_proto_.sendReliable(
                                 service::McuMsgId::MSG_SET_ECALL_LED,
                                 {static_cast<uint8_t>(mode)});
                         }
                         return {ipc::IpcStatus::OK, "{\"ok\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::PING_MCU_LINK,
                     [this](const std::string&) -> ipc::RpcResult {
                         std::vector<uint8_t> hb = {
                             static_cast<uint8_t>(power_mgr_.currentPowerState()),
                             power_mgr_.activeWakeLockBitmask(),
                             0, 0, 0, 0};
                         bool ok = mcu_hal_.isOpen() &&
                                   mcu_proto_.sendReliable(
                                       service::McuMsgId::MSG_HEARTBEAT_WDG, hb, nullptr, 150) &&
                                   mcu_proto_.isLinkHealthy();
                         common::JsonValue resp = common::JsonValue::object();
                         resp["mcu_link_ok"] = ok;
                         resp["crc_errors"]  = mcu_proto_.crcErrorCount();
                         resp["timeouts"]    = mcu_proto_.timeoutCount();
                         return {ipc::IpcStatus::OK, resp.dump(false)};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::SLEEP_READY_ACK,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             auto did = static_cast<ipc::DaemonId>((*j)["daemon_id"].asUint64(0));
                             power_mgr_.recordSleepReadyAck(did);
                             if (power_mgr_.allCoreDaemonsSleepReady() &&
                                 !power_mgr_.hasActiveWakeLocks()) {
                                 power_mgr_.setPowerState(PowerState::SLEEP);
                                 mcu_proto_.sendReliable(
                                     service::McuMsgId::MSG_POWER_TRANSITION,
                                     {static_cast<uint8_t>(PowerState::SLEEP)});
                             }
                         }
                         return {ipc::IpcStatus::OK, "{\"ack\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::INJECT_TEST_EVENT,
                     [this](const std::string& req) -> ipc::RpcResult {
                         std::vector<uint8_t> pay(req.begin(), req.end());
                         bool ok = mcu_proto_.sendReliable(
                             service::McuMsgId::MSG_SIM_INJECT_CMD, pay, nullptr, 300);
                         return {ok ? ipc::IpcStatus::OK : ipc::IpcStatus::ERR_REJECTED,
                                 ok ? "{\"ok\":true}" : "{\"ok\":false}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::GET_DAEMON_STATUS,
                     [this](const std::string&) -> ipc::RpcResult {
                         common::JsonValue j = common::JsonValue::object();
                         j["daemon"]      = "tcu-mcu-ipcd";
                         j["power_state"] = static_cast<int>(power_mgr_.currentPowerState());
                         j["wake_locks"]  = static_cast<int>(power_mgr_.activeWakeLockBitmask());
                         j["mcu_healthy"] = mcu_proto_.isLinkHealthy();
                         j["led_mode"]    = static_cast<int>(currentLedMode());
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });
}

void McuIpcOrchestrator::heartbeatLoop() {
    uint64_t last_hb_ms = 0;
    while (running_.load()) {
        const uint64_t now = steadyNowMs();
        if (now - last_hb_ms >= static_cast<uint64_t>(cfg_.mcu_link.heartbeat_interval_ms)) {
            last_hb_ms = now;
            watchdog_hal_.kick();
            if (mcu_hal_.isOpen() && power_mgr_.currentPowerState() != PowerState::SLEEP) {
                uint32_t up = static_cast<uint32_t>(now & 0xFFFFFFFFU);
                std::vector<uint8_t> pay(6, 0);
                pay[0] = static_cast<uint8_t>(power_mgr_.currentPowerState());
                pay[1] = power_mgr_.activeWakeLockBitmask();
                std::memcpy(pay.data() + 2, &up, 4);
                mcu_proto_.sendReliable(service::McuMsgId::MSG_HEARTBEAT_WDG, pay, nullptr, 100);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace tcu::app

