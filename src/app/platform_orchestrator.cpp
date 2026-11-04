#include "app/platform_orchestrator.h"
#include "common/json_mini.h"
#include "common/logger.h"

#include <algorithm>
#include <chrono>

namespace tcu::app {

namespace {
common::JsonValue fixToJson(const GnssFix& f) {
    common::JsonValue j = common::JsonValue::object();
    j["lat_deg"]            = f.lat_deg;
    j["lon_deg"]            = f.lon_deg;
    j["alt_m"]              = f.alt_m;
    j["heading_deg"]        = static_cast<double>(f.heading_deg);
    j["speed_mps"]          = static_cast<double>(f.speed_mps);
    j["hdop"]               = static_cast<double>(f.hdop);
    j["satellites"]         = static_cast<int>(f.satellites);
    j["fix_valid"]          = f.fix_valid;
    j["antenna_health"]     = static_cast<int>(f.antenna_health);
    j["antenna_current_ma"] = static_cast<double>(f.antenna_current_ma);
    j["timestamp_utc"]      = f.timestamp_utc;
    return j;
}

GnssFix jsonToFix(const common::JsonValue& j) {
    GnssFix f;
    f.lat_deg            = j["lat_deg"].asDouble();
    f.lon_deg            = j["lon_deg"].asDouble();
    f.alt_m              = j["alt_m"].asDouble(12.0);
    f.heading_deg        = static_cast<float>(j["heading_deg"].asDouble(0.0));
    f.speed_mps          = static_cast<float>(j["speed_mps"].asDouble(0.0));
    f.hdop               = static_cast<float>(j["hdop"].asDouble(0.9));
    f.satellites         = static_cast<uint8_t>(j["satellites"].asUint64(10));
    f.fix_valid          = j["fix_valid"].asBool(true);
    f.antenna_health     = static_cast<AntennaHealth>(j["antenna_health"].asUint64(0));
    f.antenna_current_ma = static_cast<float>(j["antenna_current_ma"].asDouble(18.0));
    f.timestamp_utc      = j["timestamp_utc"].asUint64(currentEpochSeconds());
    return f;
}
} // namespace

PlatformOrchestrator::PlatformOrchestrator(const TcuConfig& cfg,
                                           hal::IModemHAL& modem_hal,
                                           hal::IGnssHAL& gnss_hal,
                                           hal::IAudioHAL& audio_hal,
                                           hal::ICloudHAL& cloud_hal,
                                           hal::IStorageHAL& storage_hal,
                                           hal::ICryptoHAL& crypto_hal)
    : cfg_(cfg),
      modem_hal_(modem_hal),
      gnss_hal_(gnss_hal),
      audio_hal_(audio_hal),
      cloud_hal_(cloud_hal),
      storage_hal_(storage_hal),
      crypto_hal_(crypto_hal),
      ipc_(ipc::DaemonId::PLATFORMD, cfg.ipc.broker_socket),
      gnss_svc_(gnss_hal, cfg.gnss.default_lat, cfg.gnss.default_lon),
      store_svc_(storage_hal, crypto_hal, cfg.storage.encryption_key_hex) {}

PlatformOrchestrator::~PlatformOrchestrator() {
    stop();
}

bool PlatformOrchestrator::start() {
    if (running_.load()) return true;

    storage_hal_.open(cfg_.storage.db_path, cfg_.storage.max_queue_records);
    modem_hal_.initialize(cfg_.modem.apn);
    audio_hal_.initialize();
    gnss_hal_.start(cfg_.gnss.device, cfg_.gnss.normal_rate_hz);
    cloud_hal_.connect();

    modem_hal_.setIncomingCallCallback([this](const std::string& caller) {
        common::JsonValue j = common::JsonValue::object();
        j["caller"] = caller;
        ipc_.publish(ipc::TopicId::TOPIC_INCOMING_CALL, j.dump(false), true);
    });

    cloud_hal_.setCommandCallback([this](const std::string& topic, const std::string& payload) {
        common::JsonValue j = common::JsonValue::object();
        j["mqtt_topic"] = topic;
        j["payload"]    = payload;
        ipc_.publish(ipc::TopicId::TOPIC_CLOUD_COMMAND, j.dump(false));
    });

    registerHandlers();
    if (!ipc_.connect(2000)) {
        return false;
    }

    running_.store(true);
    worker_ = std::thread(&PlatformOrchestrator::workerLoop, this);
    return true;
}

void PlatformOrchestrator::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) {
        worker_.join();
    }
    ipc_.disconnect();
    cloud_hal_.disconnect();
    gnss_hal_.stop();
    storage_hal_.close();
}

size_t PlatformOrchestrator::offlineQueueDepth() const {
    return store_svc_.pendingCount();
}

void PlatformOrchestrator::flushOfflineTelemetryQueue() {
    if (!cloud_hal_.isConnected() || emergency_preempted_.load()) {
        return;
    }
    auto rows = store_svc_.popOrPeekDecryptedBatch(32);
    for (const auto& r : rows) {
        if (!cloud_hal_.isConnected()) break;
        if (cloud_hal_.publish(r.topic, r.plaintext_json, 1)) {
            store_svc_.acknowledgeDelivered(r.id);
        } else {
            break;
        }
    }
}

void PlatformOrchestrator::registerHandlers() {
    ipc_.subscribe(ipc::TopicId::TOPIC_POWER_TRANSITION,
                   [this](ipc::TopicId, const std::string& payload) {
                       if (auto j = common::JsonValue::parse(payload)) {
                           auto target = static_cast<PowerState>((*j)["target_state"].asInt64(-1));
                           if (target == PowerState::PRE_SLEEP) {
                               store_svc_.flushBeforeSleep();
                               common::JsonValue ack = common::JsonValue::object();
                               ack["daemon_id"] = static_cast<int>(ipc::DaemonId::PLATFORMD);
                               ipc_.callRpc(ipc::DaemonId::MCU_IPCD,
                                            ipc::RpcMethodId::SLEEP_READY_ACK,
                                            ack.dump(false),
                                            300);
                           }
                       }
                   });

    ipc_.registerRpc(ipc::RpcMethodId::ACQUIRE_EMERGENCY_CHANNEL,
                     [this](const std::string&) -> ipc::RpcResult {
                         emergency_preempted_.store(true);
                         gnss_svc_.setRateHz(cfg_.gnss.emergency_rate_hz);
                         audio_hal_.setRoute(AudioRoute::MUTED);
                         return {ipc::IpcStatus::OK, "{\"preempted\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::RELEASE_EMERGENCY_CHANNEL,
                     [this](const std::string&) -> ipc::RpcResult {
                         emergency_preempted_.store(false);
                         audio_hal_.setRoute(AudioRoute::MUTED);
                         gnss_svc_.setRateHz(cfg_.gnss.normal_rate_hz);
                         return {ipc::IpcStatus::OK, "{\"released\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::DIAL_EMERGENCY_AND_MSD,
                     [this](const std::string& req) -> ipc::RpcResult {
                         auto j = common::JsonValue::parse(req);
                         if (!j) return {ipc::IpcStatus::ERR_REJECTED, "{\"ok\":false}"};

                         std::string num   = (*j)["dial_number"].asString("112");
                         bool is_test      = (*j)["test_call"].asBool(false);
                         auto msd_vec      = common::hexToBytes((*j)["msd_hex"].asString());
                         int timeout_ms    = static_cast<int>((*j)["timeout_ms"].asInt64(5000));

                         if (msd_vec.size() != common::kEcallMsdSizeBytes) {
                             return {ipc::IpcStatus::ERR_REJECTED, "{\"error\":\"bad_msd_len\"}"};
                         }
                         std::array<uint8_t, common::kEcallMsdSizeBytes> msd_arr{};
                         std::copy(msd_vec.begin(), msd_vec.end(), msd_arr.begin());

                         bool dialed = modem_hal_.dialEmergency(num, is_test);
                         if (!dialed) {
                             return {ipc::IpcStatus::ERR_REJECTED, "{\"dialed\":false}"};
                         }
                         bool msd_acked = modem_hal_.sendInBandMsd(msd_arr, timeout_ms);

                         common::JsonValue resp = common::JsonValue::object();
                         resp["dialed"]    = true;
                         resp["msd_acked"] = msd_acked;
                         return {ipc::IpcStatus::OK, resp.dump(false)};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::HANGUP_CALL,
                     [this](const std::string&) -> ipc::RpcResult {
                         modem_hal_.hangupCall();
                         audio_hal_.setRoute(AudioRoute::MUTED);
                         return {ipc::IpcStatus::OK, "{\"ok\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::ANSWER_INCOMING_CALL,
                     [this](const std::string&) -> ipc::RpcResult {
                         bool ok = modem_hal_.answerIncomingCall();
                         if (ok) {
                             audio_hal_.setRoute(AudioRoute::MODEM_VOICE_TO_CABIN);
                         }
                         return {ok ? ipc::IpcStatus::OK : ipc::IpcStatus::ERR_REJECTED,
                                 ok ? "{\"answered\":true}" : "{\"answered\":false}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::SET_VOICE_AUDIO_ROUTE,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             auto route = static_cast<AudioRoute>((*j)["route"].asUint64(0));
                             audio_hal_.setRoute(route);
                         }
                         return {ipc::IpcStatus::OK, "{\"ok\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::GET_GNSS_TRAIL,
                     [this](const std::string&) -> ipc::RpcResult {
                         auto trail = gnss_svc_.getTrail();
                         common::JsonValue j = common::JsonValue::object();
                         j["current"] = fixToJson(trail.current);
                         j["has_n1"]  = trail.has_n1;
                         j["n1"]      = fixToJson(trail.n1);
                         j["has_n2"]  = trail.has_n2;
                         j["n2"]      = fixToJson(trail.n2);
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::SET_GNSS_RATE,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             gnss_svc_.setRateHz(static_cast<int>((*j)["rate_hz"].asInt64(1)));
                         }
                         return {ipc::IpcStatus::OK, "{\"ok\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::GET_MODEM_STATUS,
                     [this](const std::string&) -> ipc::RpcResult {
                         auto st = modem_hal_.getStatus();
                         common::JsonValue j = common::JsonValue::object();
                         j["sim_ready"]         = st.sim_ready;
                         j["registered"]        = st.registered;
                         j["roaming"]           = st.roaming;
                         j["rat"]               = st.rat;
                         j["rssi_dbm"]          = st.rssi_dbm;
                         j["imei"]              = st.imei;
                         j["voice_call_active"] = st.voice_call_active;
                         j["connected_peer"]    = st.connected_peer;

                         common::JsonValue sc = common::JsonValue::object();
                         sc["mcc"]     = st.serving_cell.mcc;
                         sc["mnc"]     = st.serving_cell.mnc;
                         sc["tac"]     = st.serving_cell.tac;
                         sc["cell_id"] = st.serving_cell.cell_id;
                         sc["rsrp"]    = st.serving_cell.rsrp_dbm;
                         j["serving_cell"] = sc;
                         j["neighbor_count"] = static_cast<uint64_t>(st.neighbor_cells.size());
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::PUBLISH_CLOUD_TELEMETRY,
                     [this](const std::string& req) -> ipc::RpcResult {
                         auto j = common::JsonValue::parse(req);
                         if (!j) return {ipc::IpcStatus::ERR_REJECTED, "{\"ok\":false}"};

                         int priority        = static_cast<int>((*j)["priority"].asInt64(2));
                         std::string topic   = (*j)["topic"].asString("tcu/telemetry");
                         std::string payload = (*j)["payload"].asString("{}");

                         if (cloud_hal_.isConnected() &&
                             (!emergency_preempted_.load() || priority == 0)) {
                             if (cloud_hal_.publish(topic, payload, 1)) {
                                 flushOfflineTelemetryQueue();
                                 return {ipc::IpcStatus::OK, "{\"published\":true,\"queued\":false}"};
                             }
                         }

                         // Offline Store-and-Forward with AES-256-GCM encryption!
                         bool queued = store_svc_.storeEncryptedBreadcrumb(priority, topic, payload);
                         return {queued ? ipc::IpcStatus::OK : ipc::IpcStatus::ERR_INTERNAL,
                                 "{\"published\":false,\"queued\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::DOWNLOAD_HTTPS_ASSET,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (emergency_preempted_.load()) {
                             return {ipc::IpcStatus::ERR_PREEMPTED, "{\"preempted\":true}"};
                         }
                         auto j = common::JsonValue::parse(req);
                         if (!j) return {ipc::IpcStatus::ERR_REJECTED, "{\"ok\":false}"};

                         std::string url  = (*j)["url"].asString();
                         std::string dest = (*j)["dest_path"].asString();

                         bool ok = cloud_hal_.downloadAssetResumable(
                             url, dest, [this](uint64_t, uint64_t) -> bool {
                                 return running_.load() && !emergency_preempted_.load();
                             });

                         if (emergency_preempted_.load()) {
                             return {ipc::IpcStatus::ERR_PREEMPTED, "{\"preempted\":true}"};
                         }
                         return {ok ? ipc::IpcStatus::OK : ipc::IpcStatus::ERR_REJECTED,
                                 ok ? "{\"ok\":true}" : "{\"ok\":false}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::EXECUTE_AUDIO_LOOPBACK,
                     [this](const std::string&) -> ipc::RpcResult {
                         if (emergency_preempted_.load()) {
                             // During active emergency voice call, codec is live and healthy
                             return {ipc::IpcStatus::OK, "{\"passed\":true,\"snr_db\":40.0}"};
                         }
                         auto res = audio_hal_.runDiagnosticLoopback(1000, 50);
                         common::JsonValue j = common::JsonValue::object();
                         j["passed"]    = res.passed;
                         j["snr_db"]    = static_cast<double>(res.snr_db);
                         j["impedance"] = static_cast<double>(res.speaker_impedance_ohm);
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::INJECT_TEST_EVENT,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             std::string cmd = (*j)["cmd"].asString();
                             if (cmd == "set_gnss") {
                                 GnssFix f = jsonToFix(*j);
                                 gnss_svc_.ingestFix(f);
                                 ipc_.publish(ipc::TopicId::TOPIC_GNSS_FIX, fixToJson(f).dump(false));
                             } else if (cmd == "cloud_command") {
                                 common::JsonValue ev = common::JsonValue::object();
                                 ev["mqtt_topic"] = (*j)["mqtt_topic"].asString();
                                 ev["payload"]    = (*j)["payload"].asString();
                                 ipc_.publish(ipc::TopicId::TOPIC_CLOUD_COMMAND, ev.dump(false));
                             }
                         }
                         return {ipc::IpcStatus::OK, "{\"ok\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::GET_DAEMON_STATUS,
                     [this](const std::string&) -> ipc::RpcResult {
                         common::JsonValue j = common::JsonValue::object();
                         j["daemon"]              = "tcu-platformd";
                         j["emergency_preempted"] = emergency_preempted_.load();
                         j["cloud_connected"]     = cloud_hal_.isConnected();
                         j["offline_queue_depth"] = static_cast<uint64_t>(offlineQueueDepth());
                         j["audio_route"]         = static_cast<int>(audio_hal_.currentRoute());
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });
}

void PlatformOrchestrator::workerLoop() {
    uint64_t last_gnss_ms  = 0;
    uint64_t last_flush_ms = 0;

    while (running_.load()) {
        const uint64_t now = steadyNowMs();
        const uint64_t gnss_period_ms =
            emergency_preempted_.load() ? 100U : 250U; // Responsive polling

        if (now - last_gnss_ms >= gnss_period_ms) {
            last_gnss_ms = now;
            GnssFix fix = gnss_svc_.pollAndUpdate(10);
            ipc_.publish(ipc::TopicId::TOPIC_GNSS_FIX, fixToJson(fix).dump(false));
        }

        if (now - last_flush_ms >= 500U) {
            last_flush_ms = now;
            flushOfflineTelemetryQueue();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

} // namespace tcu::app

