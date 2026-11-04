#include "app/ota_orchestrator.h"
#include "common/geo_math.h"
#include "common/json_mini.h"
#include "common/logger.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tcu::app {

OtaOrchestrator::OtaOrchestrator(const TcuConfig& cfg,
                                 hal::IOtaBootHAL& ota_boot_hal,
                                 hal::ICryptoHAL& crypto_hal,
                                 hal::IStorageHAL& storage_hal)
    : cfg_(cfg),
      ota_boot_hal_(ota_boot_hal),
      crypto_hal_(crypto_hal),
      storage_hal_(storage_hal),
      ipc_(ipc::DaemonId::OTAD, cfg.ipc.broker_socket) {}

OtaOrchestrator::~OtaOrchestrator() {
    stop();
}

std::string OtaOrchestrator::canonicalSigningString(const OtaManifest& m) {
    std::ostringstream oss;
    oss << m.campaign_id << "|"
        << m.bundle_version << "|"
        << m.min_anti_rollback_version << "|"
        << m.linux_bundle_filename << "|"
        << m.linux_bundle_size << "|"
        << m.linux_bundle_sha256 << "|"
        << (m.mcu_firmware_present ? "1" : "0") << "|"
        << m.mcu_firmware_version << "|"
        << m.mcu_firmware_size << "|"
        << m.mcu_firmware_crc32 << "|"
        << m.mcu_firmware_sha256;
    return oss.str();
}

std::optional<OtaManifest> OtaOrchestrator::parseManifestJson(const std::string& json_str) {
    auto j = common::JsonValue::parse(json_str);
    if (!j || !j->isObject()) return std::nullopt;

    OtaManifest m;
    m.campaign_id               = (*j)["campaign_id"].asString();
    m.bundle_version            = (*j)["bundle_version"].asString();
    m.min_anti_rollback_version = (*j)["min_anti_rollback_version"].asString("0.0.0");
    m.manifest_signature_hex    = (*j)["signature_hex"].asString();

    for (const auto& hw : (*j)["hw_compatibility"].asArray()) {
        m.hw_compatibility.push_back(hw.asString());
    }

    const auto& art = (*j)["artifacts"];
    const auto& lin = art["linux_rauc_bundle"];
    m.linux_bundle_url      = lin["url"].asString();
    m.linux_bundle_filename = lin["filename"].asString("rootfs.raucb");
    m.linux_bundle_size     = lin["size_bytes"].asUint64(0);
    m.linux_bundle_sha256   = lin["sha256"].asString();

    const auto& mcu = art["mcu_firmware"];
    m.mcu_firmware_present  = mcu["present"].asBool(false);
    m.mcu_firmware_url      = mcu["url"].asString();
    m.mcu_firmware_filename = mcu["filename"].asString("mcu.bin");
    m.mcu_firmware_version  = mcu["version"].asString("0.0.0");
    m.mcu_firmware_size     = static_cast<uint32_t>(mcu["size_bytes"].asUint64(0));
    m.mcu_firmware_crc32    = static_cast<uint32_t>(mcu["crc32"].asUint64(0));
    m.mcu_firmware_sha256   = mcu["sha256"].asString();

    if (m.campaign_id.empty() || m.bundle_version.empty()) {
        return std::nullopt;
    }
    return m;
}

std::string OtaOrchestrator::serializeManifestJson(const OtaManifest& m) {
    common::JsonValue j = common::JsonValue::object();
    j["campaign_id"]               = m.campaign_id;
    j["bundle_version"]            = m.bundle_version;
    j["min_anti_rollback_version"] = m.min_anti_rollback_version;
    j["signature_hex"]             = m.manifest_signature_hex;

    common::JsonValue hw_arr = common::JsonValue::array();
    for (const auto& h : m.hw_compatibility) hw_arr.push_back(h);
    j["hw_compatibility"] = hw_arr;

    common::JsonValue lin = common::JsonValue::object();
    lin["url"]        = m.linux_bundle_url;
    lin["filename"]   = m.linux_bundle_filename;
    lin["size_bytes"] = m.linux_bundle_size;
    lin["sha256"]     = m.linux_bundle_sha256;

    common::JsonValue mcu = common::JsonValue::object();
    mcu["present"]    = m.mcu_firmware_present;
    mcu["url"]        = m.mcu_firmware_url;
    mcu["filename"]   = m.mcu_firmware_filename;
    mcu["version"]    = m.mcu_firmware_version;
    mcu["size_bytes"] = m.mcu_firmware_size;
    mcu["crc32"]      = m.mcu_firmware_crc32;
    mcu["sha256"]     = m.mcu_firmware_sha256;

    common::JsonValue art = common::JsonValue::object();
    art["linux_rauc_bundle"] = lin;
    art["mcu_firmware"]      = mcu;
    j["artifacts"]           = art;
    return j.dump(true);
}

bool OtaOrchestrator::start() {
    if (running_.load()) return true;

    std::error_code ec;
    std::filesystem::create_directories(cfg_.ota.staging_dir, ec);
    storage_hal_.open(cfg_.storage.db_path, cfg_.storage.max_queue_records);

    registerHandlers();
    if (!ipc_.connect(2000)) {
        return false;
    }

    // Seed initial vehicle state from MCU_IPCD
    auto vres = ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::GET_VEHICLE_STATE, "{}", 300);
    if (vres.ok()) {
        if (auto vj = common::JsonValue::parse(vres.payload)) {
            std::lock_guard<std::mutex> lk(mtx_);
            latest_vehicle_.speed_kmh     = (*vj)["speed_kmh"].asDouble();
            latest_vehicle_.ignition_kl15 = (*vj)["ignition_kl15"].asBool();
            latest_vehicle_.gear          = static_cast<GearPosition>((*vj)["gear"].asInt64(0));
            latest_vehicle_.kl30_mv       = static_cast<uint16_t>((*vj)["kl30_mv"].asUint64(12600));
            latest_vehicle_.bub_soc       = static_cast<uint8_t>((*vj)["bub_soc"].asUint64(95));
        }
    }

    // Check if booted into an unverified new OTA slot requiring post-boot BIST verification
    auto slot_st = ota_boot_hal_.getSlotStatus();
    if (slot_st.pending_post_boot_verify) {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = OtaState::POST_BOOT_VERIFY;
    }

    running_.store(true);
    worker_ = std::thread(&OtaOrchestrator::workerLoop, this);
    return true;
}

void OtaOrchestrator::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    ipc_.disconnect();
    storage_hal_.close();
}

OtaState OtaOrchestrator::currentState() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return state_;
}

std::string OtaOrchestrator::lastFailureReason() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_failure_reason_;
}

void OtaOrchestrator::submitCampaign(const OtaManifest& manifest) {
    std::lock_guard<std::mutex> lk(mtx_);
    pending_campaign_ = manifest;
    cv_.notify_all();
}

bool OtaOrchestrator::submitCampaignJson(const std::string& manifest_json) {
    auto m = parseManifestJson(manifest_json);
    if (!m) return false;
    submitCampaign(*m);
    return true;
}

bool OtaOrchestrator::isVehicleSafetyGateSatisfied(const VehicleState& v) const {
    return !ecall_active_.load() &&
           (v.speed_kmh == 0.0) &&
           (v.gear == GearPosition::PARK) &&
           (!v.ignition_kl15) &&
           (v.kl30_mv >= cfg_.ota.min_battery_mv) &&
           (v.bub_soc >= cfg_.ota.min_bub_soc_pct);
}

void OtaOrchestrator::recordJournal(const OtaManifest& m,
                                    const std::string& prev_slot,
                                    const std::string& target_slot,
                                    const std::string& state_str,
                                    const std::string& reason) {
    hal::OtaJournalRow row;
    row.campaign_id      = m.campaign_id;
    row.target_version   = m.bundle_version;
    row.previous_slot    = prev_slot;
    row.target_slot      = target_slot;
    row.state            = state_str;
    row.bytes_downloaded = m.linux_bundle_size;
    row.updated_utc      = currentEpochSeconds();
    row.failure_reason   = reason;
    storage_hal_.upsertOtaJournal(row);
}

void OtaOrchestrator::publishOtaStatusReport(const std::string& campaign_id,
                                             const std::string& status_str,
                                             const std::string& detail) {
    common::JsonValue body = common::JsonValue::object();
    body["campaign_id"] = campaign_id;
    body["status"]      = status_str;
    body["detail"]      = detail;
    auto slots = ota_boot_hal_.getSlotStatus();
    body["active_slot"]       = slots.active_slot;
    body["installed_version"] = slots.installed_version;

    common::JsonValue req = common::JsonValue::object();
    req["priority"] = 2;
    req["topic"]    = "tcu/" + cfg_.system.device_id + "/ota_status";
    req["payload"]  = body.dump(false);
    ipc_.callRpc(ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::PUBLISH_CLOUD_TELEMETRY, req.dump(false), 300);
}

bool OtaOrchestrator::evaluatePostBootVerification() {
    auto slots = ota_boot_hal_.getSlotStatus();
    auto bist_res = ipc_.callRpc(ipc::DaemonId::DIAGD, ipc::RpcMethodId::RUN_BIST_NOW, "{}", 1500);

    bool bist_passed = false;
    if (bist_res.ok()) {
        if (auto j = common::JsonValue::parse(bist_res.payload)) {
            bist_passed = (*j)["all_critical_ok"].asBool(false);
        }
    }

    auto latest_j = storage_hal_.getLatestOtaJournal();
    std::string camp_id = latest_j ? latest_j->campaign_id : "OTA-POST-BOOT";

    if (bist_passed && ota_boot_hal_.markCurrentSlotGood()) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = OtaState::COMMITTED_GOOD;
        }
        if (latest_j) {
            latest_j->state       = "COMMITTED_GOOD";
            latest_j->updated_utc = currentEpochSeconds();
            storage_hal_.upsertOtaJournal(*latest_j);
        }
        publishOtaStatusReport(camp_id, "OTA_SUCCESS", "Post-boot BIST passed and slot marked good");
        return true;
    }

    // Post-boot BIST failed -> Atomic Rollback to previous working bank!
    ota_boot_hal_.rollbackToPreviousSlot();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_               = OtaState::AUTOMATIC_ROLLBACK;
        last_failure_reason_ = "post_boot_bist_failed";
    }
    if (latest_j) {
        latest_j->state          = "ROLLED_BACK";
        latest_j->failure_reason = "post_boot_bist_failed";
        latest_j->updated_utc    = currentEpochSeconds();
        storage_hal_.upsertOtaJournal(*latest_j);
    }
    publishOtaStatusReport(camp_id, "OTA_ROLLED_BACK", "Post-boot BIST failed; reverted to healthy bank");
    return false;
}

void OtaOrchestrator::processCampaign(const OtaManifest& m) {
    const auto init_slots = ota_boot_hal_.getSlotStatus();

    auto abortWith = [&](const std::string& reason) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_               = OtaState::ABORTED_REPORT;
            last_failure_reason_ = reason;
        }
        recordJournal(m, init_slots.active_slot, init_slots.inactive_slot, "ABORTED", reason);
        publishOtaStatusReport(m.campaign_id, "OTA_ABORTED", reason);
    };

    // Step 1: Hardware Compatibility & Anti-Rollback Pre-Checks
    bool hw_ok = false;
    for (const auto& hw : m.hw_compatibility) {
        if (hw == cfg_.system.hw_compat || hw == "qemuarm64-tcu" || hw == "*") {
            hw_ok = true;
            break;
        }
    }
    if (!hw_ok) {
        abortWith("incompatible_hardware");
        return;
    }

    if (common::compareSemVer(m.bundle_version, cfg_.ota.min_anti_rollback_version) < 0 ||
        common::compareSemVer(m.bundle_version, init_slots.installed_version) <= 0) {
        abortWith("anti_rollback_version_violation");
        return;
    }

    // Step 2: Resumable HTTPS / Asset Download (with P0 eCall Preemption awareness)
    const std::string linux_dest = cfg_.ota.staging_dir + "/" + m.linux_bundle_filename;
    const std::string mcu_dest   = cfg_.ota.staging_dir + "/" + m.mcu_firmware_filename;

    auto downloadWithResume = [&](const std::string& url, const std::string& dest) -> bool {
        while (running_.load()) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                state_ = OtaState::DOWNLOADING;
            }
            common::JsonValue dreq = common::JsonValue::object();
            dreq["url"]       = url;
            dreq["dest_path"] = dest;
            auto dres = ipc_.callRpc(
                ipc::DaemonId::PLATFORMD, ipc::RpcMethodId::DOWNLOAD_HTTPS_ASSET, dreq.dump(false), 5000);
            if (dres.ok()) {
                return true;
            }
            if (dres.status == ipc::IpcStatus::ERR_PREEMPTED || ecall_active_.load()) {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    state_ = OtaState::DOWNLOAD_PAUSED;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            return false;
        }
        return false;
    };

    recordJournal(m, init_slots.active_slot, init_slots.inactive_slot, "DOWNLOADING");
    if (!downloadWithResume(m.linux_bundle_url, linux_dest)) {
        abortWith("linux_bundle_download_failed");
        return;
    }
    if (m.mcu_firmware_present && !downloadWithResume(m.mcu_firmware_url, mcu_dest)) {
        abortWith("mcu_firmware_download_failed");
        return;
    }

    // Step 3: Cryptographic Signature & SHA-256 Digest Verification
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = OtaState::VERIFYING;
    }

    const std::string canonical = canonicalSigningString(m);
    const auto sig_bytes        = common::hexToBytes(m.manifest_signature_hex);
    if (!crypto_hal_.verifySignature(canonical, sig_bytes, cfg_.ota.public_key_pem)) {
        abortWith("signature_verification_failed");
        return;
    }

    const std::string calc_linux_sha = crypto_hal_.sha256FileHex(linux_dest);
    if (calc_linux_sha != m.linux_bundle_sha256) {
        abortWith("linux_bundle_sha256_mismatch");
        return;
    }

    std::vector<uint8_t> mcu_bytes;
    if (m.mcu_firmware_present) {
        const std::string calc_mcu_sha = crypto_hal_.sha256FileHex(mcu_dest);
        if (calc_mcu_sha != m.mcu_firmware_sha256) {
            abortWith("mcu_firmware_sha256_mismatch");
            return;
        }
        std::ifstream mifs(mcu_dest, std::ios::binary);
        mcu_bytes.assign(std::istreambuf_iterator<char>(mifs), std::istreambuf_iterator<char>());
        if (mcu_bytes.size() != m.mcu_firmware_size) {
            abortWith("mcu_firmware_size_mismatch");
            return;
        }
    }

    recordJournal(m, init_slots.active_slot, init_slots.inactive_slot, "VERIFIED");

    // Step 4: Wait for Vehicle Safety Preconditions (Speed=0, PARK, IGN=OFF, KL30>=12.2V, No eCall)
    {
        std::unique_lock<std::mutex> lk(mtx_);
        state_ = OtaState::WAITING_SAFETY_GATE;
        while (running_.load() && !isVehicleSafetyGateSatisfied(latest_vehicle_)) {
            cv_.wait_for(lk, std::chrono::milliseconds(50));
        }
        if (!running_.load()) return;
    }

    // Acquire bounded OTA WakeLock so MCU does not power off SA515M mid-flash
    common::JsonValue wlk = common::JsonValue::object();
    wlk["daemon_id"] = static_cast<int>(ipc::DaemonId::OTAD);
    wlk["reason"]    = "OTA_FLASH";
    ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::REQUEST_POWER_LOCK, wlk.dump(false), 300);

    auto releaseOtaLock = [&]() {
        ipc_.callRpc(ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::RELEASE_POWER_LOCK, wlk.dump(false), 300);
    };

    // Step 5: Flash Companion MCU Inactive Flash Bank over CRC-Protected SPI (if present)
    if (m.mcu_firmware_present) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = OtaState::FLASHING_MCU;
        }
        int maj = 0, min = 0, pat = 0;
        std::sscanf(m.mcu_firmware_version.c_str(), "%d.%d.%d", &maj, &min, &pat);
        uint32_t ver_code = (static_cast<uint32_t>(maj) << 16U) |
                            (static_cast<uint32_t>(min) << 8U) |
                            static_cast<uint32_t>(pat);

        common::JsonValue mreq = common::JsonValue::object();
        mreq["fw_hex"]          = common::bytesToHex(mcu_bytes);
        mreq["fw_version_code"] = ver_code;

        auto mres = ipc_.callRpc(
            ipc::DaemonId::MCU_IPCD, ipc::RpcMethodId::FLASH_MCU_FIRMWARE_BANK, mreq.dump(false), 15000);
        if (!mres.ok()) {
            releaseOtaLock();
            abortWith("mcu_dfu_flash_failed");
            return;
        }
        recordJournal(m, init_slots.active_slot, init_slots.inactive_slot, "FLASHED_MCU");
    }

    // Step 6: Install RAUC Bundle to Inactive Linux Partition (Slot A -> Slot B)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = OtaState::INSTALLING_LINUX_AB;
    }
    if (!ota_boot_hal_.installBundleToInactiveSlot(linux_dest, m.bundle_version)) {
        releaseOtaLock();
        abortWith("rauc_slot_install_failed");
        return;
    }

    // Step 7: Stage Boot Order & Reboot into Newly Installed Slot
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = OtaState::REBOOTING_SLOT;
    }
    recordJournal(m, init_slots.active_slot, init_slots.inactive_slot, "REBOOTING");
    ota_boot_hal_.triggerRebootIntoStagedSlot();
    releaseOtaLock();

    // Step 8: Post-Boot Verification via tcu-diagd BIST (Commit mark-good or Automatic Rollback)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = OtaState::POST_BOOT_VERIFY;
    }
    evaluatePostBootVerification();
}

void OtaOrchestrator::registerHandlers() {
    ipc_.subscribe(ipc::TopicId::TOPIC_VEHICLE_STATE,
                   [this](ipc::TopicId, const std::string& payload) {
                       if (auto j = common::JsonValue::parse(payload)) {
                           std::lock_guard<std::mutex> lk(mtx_);
                           latest_vehicle_.speed_kmh     = (*j)["speed_kmh"].asDouble();
                           latest_vehicle_.ignition_kl15 = (*j)["ignition_kl15"].asBool();
                           latest_vehicle_.gear          = static_cast<GearPosition>((*j)["gear"].asInt64(0));
                           latest_vehicle_.kl30_mv       = static_cast<uint16_t>((*j)["kl30_mv"].asUint64(12600));
                           latest_vehicle_.bub_soc       = static_cast<uint8_t>((*j)["bub_soc"].asUint64(95));
                           cv_.notify_all();
                       }
                   });

    ipc_.subscribe(ipc::TopicId::TOPIC_EMERGENCY_TRIGGER,
                   [this](ipc::TopicId, const std::string&) {
                       ecall_active_.store(true);
                       cv_.notify_all();
                   });

    ipc_.subscribe(ipc::TopicId::TOPIC_CLOUD_COMMAND,
                   [this](ipc::TopicId, const std::string& payload) {
                       if (auto ev = common::JsonValue::parse(payload)) {
                           if ((*ev)["mqtt_topic"].asString().find("ota") != std::string::npos) {
                               submitCampaignJson((*ev)["payload"].asString());
                           }
                       }
                   });

    ipc_.registerRpc(ipc::RpcMethodId::TRIGGER_OTA_CAMPAIGN,
                     [this](const std::string& req) -> ipc::RpcResult {
                         bool ok = submitCampaignJson(req);
                         return {ok ? ipc::IpcStatus::OK : ipc::IpcStatus::ERR_REJECTED,
                                 ok ? "{\"accepted\":true}" : "{\"accepted\":false}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::COMMIT_POST_BOOT_OTA,
                     [this](const std::string&) -> ipc::RpcResult {
                         bool ok = evaluatePostBootVerification();
                         return {ipc::IpcStatus::OK,
                                 ok ? "{\"committed\":true}" : "{\"committed\":false,\"rolled_back\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::INJECT_TEST_EVENT,
                     [this](const std::string& req) -> ipc::RpcResult {
                         if (auto j = common::JsonValue::parse(req)) {
                             if ((*j)["cmd"].asString() == "clear_ecall_flag") {
                                 ecall_active_.store(false);
                                 cv_.notify_all();
                             }
                         }
                         return {ipc::IpcStatus::OK, "{\"ok\":true}"};
                     });

    ipc_.registerRpc(ipc::RpcMethodId::GET_DAEMON_STATUS,
                     [this](const std::string&) -> ipc::RpcResult {
                         auto slots = ota_boot_hal_.getSlotStatus();
                         common::JsonValue j = common::JsonValue::object();
                         j["daemon"]            = "tcu-otad";
                         j["state"]             = static_cast<int>(currentState());
                         j["active_slot"]       = slots.active_slot;
                         j["inactive_slot"]     = slots.inactive_slot;
                         j["installed_version"] = slots.installed_version;
                         j["marked_good"]       = slots.active_marked_good;
                         j["rollback_count"]    = slots.rollback_count;
                         j["last_failure"]      = lastFailureReason();
                         return {ipc::IpcStatus::OK, j.dump(false)};
                     });
}

void OtaOrchestrator::workerLoop() {
    while (running_.load()) {
        std::optional<OtaManifest> next_campaign;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !running_.load() || pending_campaign_.has_value();
            });
            if (!running_.load()) break;
            if (pending_campaign_.has_value()) {
                next_campaign = *pending_campaign_;
                pending_campaign_.reset();
            }
        }

        if (next_campaign.has_value()) {
            processCampaign(*next_campaign);
        }
    }
}

} // namespace tcu::app

