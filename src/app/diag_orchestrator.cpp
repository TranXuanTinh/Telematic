#include "app/diag_orchestrator.h"
#include "common/json_mini.h"
#include "common/logger.h"

#include <chrono>

namespace tcu::app {

DiagOrchestrator::DiagOrchestrator(const TcuConfig& cfg,
                                   hal::IStorageHAL& storage_hal,
                                   hal::ICryptoHAL& crypto_hal,
                                   hal::IOtaBootHAL& ota_boot_hal,
                                   hal::IWatchdogHAL& watchdog_hal)
    : cfg_(cfg),
      storage_hal_(storage_hal),
      crypto_hal_(crypto_hal),
      ota_boot_hal_(ota_boot_hal),
      watchdog_hal_(watchdog_hal),
      ipc_(ipc::DaemonId::DIAGD, cfg.ipc.broker_socket),
      store_svc_(storage_hal, crypto_hal, cfg.storage.encryption_key_hex),
      uds_svc_(
          [this]() { return gatherLiveUdsSnapshot(); },
          [this](uint8_t mask) { return store_svc_.getDtcs(mask); },
          [this]() { return store_svc_.clearDtcs(); },
          [this](const std::string& new_vin) {
              std::lock_guard<std::mutex> lk(mtx_);
              active_vin_ = new_vin;
              return true;
          },
          [this](uint16_t rid, std::vector<uint8_t>& out_info) {
              return handleUdsRoutine(rid, out_info);
          }),
      active_vin_(cfg.system.vin) {}

DiagOrchestrator::~DiagOrchestrator() {
    stop();
}

bool DiagOrchestrator::start() {
    if (running_.load()) return true;

    storage_hal_.open(cfg_.storage.db_path, cfg_.storage.max_queue_records);
    watchdog_hal_.open(cfg_.diagnostics.watchdog_device, cfg_.diagnostics.watchdog_timeout_sec);

    registerHandlers();
    if (!ipc_.connect(2000)) {
        return false;
    }

    watchdog_hal_.notifySystemdReady();
    running_.store(true);

    // Run initial startup BIST per FR-DIAG-01
    runFullBist();

    worker_ = std::thread(&DiagOrchestrator::bistLoop, this);
    return true;
}

void DiagOrchestrator::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) {
        worker_.join();
    }
    watchdog_hal_.closeGracefully();
    ipc_.disconnect();
    storage_hal_.close();
}

BistSummary DiagOrchestrator::lastBistSummary() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_bist_;
}

std::vector<uint8_t> DiagOrchestrator::processUdsRequest(const std::vector<uint8_t>& uds_req) {
    return uds_svc_.handleRequest(uds_req);
}

BistSummary DiagOrchestrator::runFullBist() {
    BistSummary b;
    b.timestamp_utc = currentEpochSeconds();

    // Refresh latest vehicle state from MCU_IPCD
    VehicleState v_snap;
    auto vres = ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::GET_VEHICLE_STATE, "{}", 200);
    if (vres.ok()) {
        if (auto vj = common::JsonValue::parse(vres.payload)) {
            v_snap.speed_kmh              = (*vj)["speed_kmh"].asDouble();
            v_snap.ignition_kl15          = (*vj)["ignition_kl15"].asBool();
            v_snap.kl30_mv                = static_cast<uint16_t>((*vj)["kl30_mv"].asUint64(12600));
            v_snap.bub_mv                 = static_cast<uint16_t>((*vj)["bub_mv"].asUint64(4100));
            v_snap.bub_soc                = static_cast<uint8_t>((*vj)["bub_soc"].asUint64(95));
            v_snap.bub_soh                = static_cast<uint8_t>((*vj)["bub_soh"].asUint64(98));
            v_snap.starter_inhibit_active = (*vj)["starter_inhibit_active"].asBool(false);
            std::lock_guard<std::mutex> lk(mtx_);
            latest_vehicle_ = v_snap;
        }
    }

    // 1. Companion MCU SPI/UART Link Health
    auto mcu_ping = ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::PING_MCU_LINK, "{}", 250);
    b.mcu_ipc_ok = false;
    if (mcu_ping.ok()) {
        if (auto mj = common::JsonValue::parse(mcu_ping.payload)) {
            b.mcu_ipc_ok = (*mj)["mcu_link_ok"].asBool(false);
        }
    }
    if (!b.mcu_ipc_ok) {
        store_svc_.recordFaultDtc("U0100-87", v_snap);
    }

    // 2. Cellular Modem & SIM Readiness
    auto mdm_res = ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::GET_MODEM_STATUS, "{}", 250);
    b.modem_ok = false;
    if (mdm_res.ok()) {
        if (auto j = common::JsonValue::parse(mdm_res.payload)) {
            b.modem_ok = (*j)["sim_ready"].asBool(false);
        }
    }

    // 3. GNSS Active Antenna Current & Fault Check
    auto gnss_res = ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::GET_GNSS_TRAIL, "{}", 250);
    b.gnss_antenna_ok = true;
    if (gnss_res.ok()) {
        if (auto gj = common::JsonValue::parse(gnss_res.payload)) {
            const auto& cur  = (*gj)["current"];
            auto ant_health  = static_cast<AntennaHealth>(cur["antenna_health"].asUint64(0));
            float ant_ma     = static_cast<float>(cur["antenna_current_ma"].asDouble(18.0));

            if (ant_health == AntennaHealth::OPEN_CIRCUIT || ant_ma < cfg_.diagnostics.gnss_ant_min_ma) {
                b.gnss_antenna_ok = false;
                store_svc_.recordFaultDtc("B1A01-13", v_snap);
            } else if (ant_health == AntennaHealth::SHORT_CIRCUIT ||
                       ant_ma > cfg_.diagnostics.gnss_ant_max_ma) {
                b.gnss_antenna_ok = false;
                store_svc_.recordFaultDtc("B1A01-11", v_snap);
            }
        }
    } else {
        b.gnss_antenna_ok = false;
    }

    // 4. eCall Cabin Audio Codec Loopback Self-Test
    auto aud_res = ipc_.callRpc(
        ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::EXECUTE_AUDIO_LOOPBACK, "{}", 400);
    b.audio_loopback_ok = false;
    if (aud_res.ok()) {
        if (auto aj = common::JsonValue::parse(aud_res.payload)) {
            b.audio_loopback_ok = (*aj)["passed"].asBool(false);
        }
    }
    if (!b.audio_loopback_ok) {
        store_svc_.recordFaultDtc("B1A03-01", v_snap);
    }

    // 5. Backup Battery (BUB) State-of-Health Check
    b.bub_ok = (v_snap.bub_soh >= cfg_.diagnostics.min_bub_soh_pct);
    if (!b.bub_ok) {
        store_svc_.recordFaultDtc("B1A02-01", v_snap);
    }

    // 6. SQLite Storage & Forced Failure Injection Check
    b.storage_ok       = storage_hal_.isHealthy();
    b.ota_slot_healthy = !force_bist_fail_.load();

    auto dtcs          = store_svc_.getDtcs(0x09);
    b.active_dtc_count = static_cast<uint16_t>(dtcs.size());

    // Update cabin eCall Readiness / Fault LED (unless SVT Stealth or Active eCall overrides it)
    bool svt_stealth = false;
    auto svt_st = ipc_.callRpc(ipc::DaemonId::SVTD, ipc::RpcMethodId::GET_DAEMON_STATUS, "{}", 100);
    if (svt_st.ok()) {
        if (auto sj = common::JsonValue::parse(svt_st.payload)) {
            int s = static_cast<int>((*sj)["state"].asInt64(0));
            if (s >= static_cast<int>(SvtState::STOLEN_ACTIVE)) svt_stealth = true;
        }
    }

    bool ecall_busy = false;
    auto ec_st = ipc_.callRpc(ipc::DaemonId::ECALLD, ipc::RpcMethodId::GET_DAEMON_STATUS, "{}", 100);
    if (ec_st.ok()) {
        if (auto ej = common::JsonValue::parse(ec_st.payload)) {
            int es = static_cast<int>((*ej)["state"].asInt64(0));
            if (es != static_cast<int>(EcallState::IDLE_ARMED)) ecall_busy = true;
        }
    }

    if (svt_stealth) {
        b.ecall_led = EcallLedMode::OFF;
    } else if (ecall_busy) {
        b.ecall_led = EcallLedMode::BLINK_CALL;
    } else {
        b.ecall_led = b.allCriticalOk() ? EcallLedMode::GREEN_READY : EcallLedMode::RED_FAULT;
        common::JsonValue led = common::JsonValue::object();
        led["led_mode"] = static_cast<int>(b.ecall_led);
        ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::SET_ECALL_LED, led.dump(false), 150);
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        last_bist_ = b;
    }

    common::JsonValue pub = common::JsonValue::object();
    pub["mcu_ipc_ok"]        = b.mcu_ipc_ok;
    pub["modem_ok"]          = b.modem_ok;
    pub["gnss_antenna_ok"]   = b.gnss_antenna_ok;
    pub["audio_loopback_ok"] = b.audio_loopback_ok;
    pub["storage_ok"]        = b.storage_ok;
    pub["bub_ok"]            = b.bub_ok;
    pub["all_critical_ok"]   = b.allCriticalOk();
    pub["active_dtc_count"]  = b.active_dtc_count;
    pub["ecall_led"]         = static_cast<int>(b.ecall_led);
    ipc_.publish(ipc::TopicId::TOPIC_BIST_HEALTH, pub.dump(false));

    return b;
}

service::UdsContextSnapshot DiagOrchestrator::gatherLiveUdsSnapshot() {
    service::UdsContextSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap.vin     = active_vin_;
        snap.vehicle = latest_vehicle_;
    }
    snap.ota_slots  = ota_boot_hal_.getSlotStatus();
    snap.sw_version = snap.ota_slots.installed_version;

    auto vres = ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::GET_VEHICLE_STATE, "{}", 150);
    if (vres.ok()) {
        if (auto vj = common::JsonValue::parse(vres.payload)) {
            snap.vehicle.kl30_mv                = static_cast<uint16_t>((*vj)["kl30_mv"].asUint64(12600));
            snap.vehicle.bub_mv                 = static_cast<uint16_t>((*vj)["bub_mv"].asUint64(4100));
            snap.vehicle.bub_soc                = static_cast<uint8_t>((*vj)["bub_soc"].asUint64(95));
            snap.vehicle.bub_soh                = static_cast<uint8_t>((*vj)["bub_soh"].asUint64(98));
            snap.vehicle.starter_inhibit_active = (*vj)["starter_inhibit_active"].asBool(false);
        }
    }

    auto gres = ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::GET_GNSS_TRAIL, "{}", 150);
    if (gres.ok()) {
        if (auto gj = common::JsonValue::parse(gres.payload)) {
            const auto& c = (*gj)["current"];
            snap.gnss.lat_deg        = c["lat_deg"].asDouble();
            snap.gnss.lon_deg        = c["lon_deg"].asDouble();
            snap.gnss.satellites     = static_cast<uint8_t>(c["satellites"].asUint64(10));
            snap.gnss.hdop           = static_cast<float>(c["hdop"].asDouble(1.0));
            snap.gnss.antenna_health = static_cast<AntennaHealth>(c["antenna_health"].asUint64(0));
        }
    }

    auto mres = ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::GET_MODEM_STATUS, "{}", 150);
    if (mres.ok()) {
        if (auto mj = common::JsonValue::parse(mres.payload)) {
            snap.modem.rat        = (*mj)["rat"].asString("LTE");
            snap.modem.rssi_dbm   = static_cast<int16_t>((*mj)["rssi_dbm"].asInt64(-75));
            snap.modem.registered = (*mj)["registered"].asBool(true);
            snap.modem.imei       = (*mj)["imei"].asString();
        }
    }

    auto sres = ipc_.callRpc(ipc::DaemonId::SVTD, ipc::RpcMethodId::GET_DAEMON_STATUS, "{}", 150);
    if (sres.ok()) {
        if (auto sj = common::JsonValue::parse(sres.payload)) {
            snap.svt_state = static_cast<SvtState>((*sj)["state"].asUint64(0));
        }
    }

    return snap;
}

bool DiagOrchestrator::handleUdsRoutine(uint16_t routine_id, std::vector<uint8_t>& out_info) {
    switch (routine_id) {
        case 0x0201: { // Start Non-Emergency Test eCall (UN R144 Test Mode)
            common::JsonValue j = common::JsonValue::object();
            j["automatic_activation"] = false;
            j["test_call"]            = true;
            j["airbag_mask"]          = 0;
            j["delta_v_x_kmh"]        = 0;
            j["rollover"]             = false;
            j["timestamp_utc"]        = currentEpochSeconds();
            ipc_.publish(ipc::TopicId::TOPIC_EMERGENCY_TRIGGER, j.dump(false), true);
            out_info = {0x01}; // Routine started
            return true;
        }
        case 0x0202: { // Codec Audio Loopback Self-Test
            auto res = ipc_.callRpc(
                ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::EXECUTE_AUDIO_LOOPBACK, "{}", 500);
            bool passed = false;
            if (res.ok()) {
                if (auto j = common::JsonValue::parse(res.payload)) {
                    passed = (*j)["passed"].asBool(false);
                }
            }
            out_info = {passed ? uint8_t{0x00} : uint8_t{0x01}}; // 0x00 = PASS, 0x01 = FAIL
            return true;
        }
        case 0x0203: { // Full Subsystem BIST
            auto bist = runFullBist();
            uint8_t mask = 0U;
            if (bist.mcu_ipc_ok)        mask |= 0x01U;
            if (bist.modem_ok)          mask |= 0x02U;
            if (bist.gnss_antenna_ok)   mask |= 0x04U;
            if (bist.audio_loopback_ok) mask |= 0x08U;
            if (bist.storage_ok)        mask |= 0x10U;
            if (bist.bub_ok)            mask |= 0x20U;
            out_info = {bist.allCriticalOk() ? uint8_t{0x00} : uint8_t{0x01}, mask};
            return true;
        }
        default:
            return false;
    }
}

void DiagOrchestrator::registerHandlers() {
    ipc_.subscribe(ipc::TopicId::TOPIC_VEHICLE_STATE,
                   [this](ipc::TopicId, const std::string& payload) {
                       if (auto j = common::JsonValue::parse(payload)) {
                           std::lock_guard<std::mutex> lk(mtx_);
                           latest_vehicle_.speed_kmh     = (*j)["speed_kmh"].asDouble();
                           latest_vehicle_.ignition_kl15 = (*j)["ignition_kl15"].asBool();
                           latest_vehicle_.kl30_mv       = static_cast<uint16_t>((*j)["kl30_mv"].asUint64(12600));
                           latest_vehicle_.bub_mv        = static_cast<uint16_t>((*j)["bub_mv"].asUint64(4100));
                           latest_vehicle_.bub_soc       = static_cast<uint8_t>((*j)["bub_soc"].asUint64(95));
                           latest_vehicle_.bub_soh       = static_cast<uint8_t>((*j)["bub_soh"].asUint64(98));
                       }
                   });

    ipc_.registerRpc(ipc::RpcMethodId::RUN_BIST_NOW,
                     [this](const std::string&) -> ipc::RpcResult {
                         auto b = runFullBist();
                         common::JsonValue j = common::JsonValue::object();
                         j["mcu_ipc_ok"]        = b.mcu_ipc_ok;
                         j["modem_ok"]          = b.modem_ok;
                         j["gnss_antenna_ok"]   = b.gnss_antenna_ok;
                         j["audio_loopback_ok"] = b.audio_loopback_ok;
                         j["storage_ok"]        = b.storage_ok;
                         j["bub_ok"]            = b.bub_ok;
                         j["ota_slot_healthy"]  = b.ota_slot_healthy;
                         j["all_critical_ok"]   = b.allCriticalOk();
                         j["active_dtc_count"]  = b.active_dtc_count;
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::PROCESS_UDS_PDU,
                     [this](const std::string& req) -> ipc::RpcResult {
                         auto j = common::JsonValue::parse(req);
                         if (!j) return {ipc::IpcStatus::ERR_REJECTED, "{\"error\":\"bad_json\"}"};
                         auto req_bytes  = common::hexToBytes((*j)["pdu_hex"].asString());
                         auto resp_bytes = processUdsRequest(req_bytes);
                         common::JsonValue rj = common::JsonValue::object();
                         rj["pdu_hex"] = common::bytesToHex(resp_bytes);
                         return {ipc::IpcStatus::OK, rj.dump(false)};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::INJECT_TEST_EVENT,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             if ((*j)["cmd"].asString() == "force_bist_fail") {
                                 setSimulatedPostBootFailure((*j)["fail"].asBool(true));
                             }
                         }
                         return {ipc::IpcStatus::OK, "{\"ok\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::GET_DAEMON_STATUS,
                     [this](const std::string&) -> ipc::RpcResult {
                         auto b = lastBistSummary();
                         common::JsonValue j = common::JsonValue::object();
                         j["daemon"]           = "tcu-diagd";
                         j["all_critical_ok"]  = b.allCriticalOk();
                         j["active_dtc_count"] = b.active_dtc_count;
                         j["uds_session"]      = static_cast<int>(uds_svc_.activeSession());
                         j["ecall_led"]        = static_cast<int>(b.ecall_led);
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });
}

void DiagOrchestrator::bistLoop() {
    uint64_t last_bist_ms = steadyNowMs();
    uint64_t last_wdg_ms  = steadyNowMs();

    while (running_.load()) {
        const uint64_t now = steadyNowMs();
        if (now - last_wdg_ms >= 5000U) {
            last_wdg_ms = now;
            watchdog_hal_.kick();
        }
        if (now - last_bist_ms >= static_cast<uint64_t>(cfg_.diagnostics.bist_interval_sec) * 1000ULL) {
            last_bist_ms = now;
            runFullBist();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace tcu::app

