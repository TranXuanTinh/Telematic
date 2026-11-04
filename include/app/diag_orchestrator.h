#pragma once

#include "common/types.h"
#include "hal/i_crypto_hal.h"
#include "hal/i_ota_boot_hal.h"
#include "hal/i_storage_hal.h"
#include "hal/i_watchdog_hal.h"
#include "ipc/ipc_client.h"
#include "service/persistent_store_service.h"
#include "service/uds_diagnostics_service.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tcu::app {

/// Tier 1 Feature Orchestrator for tcu-diagd (Test Service, BIST, DTCs & ISO 14229 UDS).
class DiagOrchestrator {
public:
    DiagOrchestrator(const TcuConfig& cfg,
                     hal::IStorageHAL& storage_hal,
                     hal::ICryptoHAL& crypto_hal,
                     hal::IOtaBootHAL& ota_boot_hal,
                     hal::IWatchdogHAL& watchdog_hal);
    ~DiagOrchestrator();

    bool start();
    void stop();

    /// Execute full subsystem Built-In Self-Test (MCU, Modem, GNSS Antenna, Audio, BUB, Storage).
    BistSummary runFullBist();
    BistSummary lastBistSummary() const;

    /// Process raw ISO 14229 UDS request bytes and return UDS response bytes.
    std::vector<uint8_t> processUdsRequest(const std::vector<uint8_t>& uds_req);

    /// Override simulated BIST failure flag (for testing OTA Post-Boot Rollback).
    void setSimulatedPostBootFailure(bool force_fail) { force_bist_fail_.store(force_fail); }

private:
    void registerHandlers();
    void bistLoop();
    service::UdsContextSnapshot gatherLiveUdsSnapshot();
    bool handleUdsRoutine(uint16_t routine_id, std::vector<uint8_t>& out_info);

    TcuConfig                        cfg_;
    hal::IStorageHAL&                storage_hal_;
    hal::ICryptoHAL&                 crypto_hal_;
    hal::IOtaBootHAL&                ota_boot_hal_;
    hal::IWatchdogHAL&               watchdog_hal_;

    ipc::IpcClient                   ipc_;
    service::PersistentStoreService  store_svc_;
    service::UdsDiagnosticsService   uds_svc_;

    std::atomic<bool>                running_{false};
    std::atomic<bool>                force_bist_fail_{false};
    std::thread                      worker_;

    mutable std::mutex               mtx_;
    std::string                      active_vin_;
    VehicleState                     latest_vehicle_{};
    BistSummary                      last_bist_{};
};

} // namespace tcu::app

