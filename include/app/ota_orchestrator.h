#pragma once

#include "common/types.h"
#include "hal/i_crypto_hal.h"
#include "hal/i_ota_boot_hal.h"
#include "hal/i_storage_hal.h"
#include "ipc/ipc_client.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace tcu::app {

/// Tier 1 Feature Orchestrator for tcu-otad (ISO 24089 / UN ECE R156).
/// Orchestrates Dual-Domain OTA: Companion MCU Dual-Bank SPI DFU + Yocto RAUC A/B RootFS.
class OtaOrchestrator {
public:
    OtaOrchestrator(const TcuConfig& cfg,
                    hal::IOtaBootHAL& ota_boot_hal,
                    hal::ICryptoHAL& crypto_hal,
                    hal::IStorageHAL& storage_hal);
    ~OtaOrchestrator();

    bool start();
    void stop();

    OtaState currentState() const;
    std::string lastFailureReason() const;

    /// Trigger an OTA campaign from parsed manifest or JSON string.
    void submitCampaign(const OtaManifest& manifest);
    bool submitCampaignJson(const std::string& manifest_json);

    /// Execute post-boot verification against tcu-diagd BIST: commits mark-good or rolls back!
    bool evaluatePostBootVerification();

    /// Canonical string representation signed by OEM Release Signing Key.
    static std::string canonicalSigningString(const OtaManifest& m);
    static std::optional<OtaManifest> parseManifestJson(const std::string& json_str);
    static std::string serializeManifestJson(const OtaManifest& m);

private:
    void registerHandlers();
    void workerLoop();
    void processCampaign(const OtaManifest& m);
    bool isVehicleSafetyGateSatisfied(const VehicleState& v) const;
    void recordJournal(const OtaManifest& m,
                       const std::string& prev_slot,
                       const std::string& target_slot,
                       const std::string& state_str,
                       const std::string& reason = "");
    void publishOtaStatusReport(const std::string& campaign_id,
                                const std::string& status_str,
                                const std::string& detail = "");

    TcuConfig                   cfg_;
    hal::IOtaBootHAL&           ota_boot_hal_;
    hal::ICryptoHAL&            crypto_hal_;
    hal::IStorageHAL&           storage_hal_;
    ipc::IpcClient              ipc_;

    std::atomic<bool>           running_{false};
    std::atomic<bool>           ecall_active_{false};
    std::thread                 worker_;

    mutable std::mutex          mtx_;
    std::condition_variable     cv_;
    OtaState                    state_{OtaState::IDLE};
    VehicleState                latest_vehicle_{};
    std::optional<OtaManifest>  pending_campaign_;
    std::string                 last_failure_reason_;
};

} // namespace tcu::app

