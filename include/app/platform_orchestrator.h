#pragma once

#include "common/types.h"
#include "hal/i_audio_hal.h"
#include "hal/i_cloud_hal.h"
#include "hal/i_crypto_hal.h"
#include "hal/i_gnss_hal.h"
#include "hal/i_modem_hal.h"
#include "hal/i_storage_hal.h"
#include "ipc/ipc_client.h"
#include "service/gnss_service.h"
#include "service/persistent_store_service.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace tcu::app {

/// Tier 1 Orchestrator for tcu-platformd:
/// Manages Cellular Modem, Multi-Constellation GNSS Trail, Cabin Audio, mTLS Cloud & SQLite Queue.
class PlatformOrchestrator {
public:
    PlatformOrchestrator(const TcuConfig& cfg,
                         hal::IModemHAL& modem_hal,
                         hal::IGnssHAL& gnss_hal,
                         hal::IAudioHAL& audio_hal,
                         hal::ICloudHAL& cloud_hal,
                         hal::IStorageHAL& storage_hal,
                         hal::ICryptoHAL& crypto_hal);
    ~PlatformOrchestrator();

    bool start();
    void stop();

    bool isEmergencyPreempted() const noexcept { return emergency_preempted_.load(); }
    size_t offlineQueueDepth() const;

private:
    void registerHandlers();
    void workerLoop();
    void flushOfflineTelemetryQueue();

    TcuConfig                        cfg_;
    hal::IModemHAL&                  modem_hal_;
    hal::IGnssHAL&                   gnss_hal_;
    hal::IAudioHAL&                  audio_hal_;
    hal::ICloudHAL&                  cloud_hal_;
    hal::IStorageHAL&                storage_hal_;
    hal::ICryptoHAL&                 crypto_hal_;

    ipc::IpcClient                   ipc_;
    service::GnssService             gnss_svc_;
    service::PersistentStoreService  store_svc_;

    std::atomic<bool>                running_{false};
    std::atomic<bool>                emergency_preempted_{false};
    std::thread                      worker_;
};

} // namespace tcu::app

