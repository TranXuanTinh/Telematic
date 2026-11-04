#pragma once

#include "app/diag_orchestrator.h"
#include "app/ecall_orchestrator.h"
#include "app/mcu_ipc_orchestrator.h"
#include "app/mcu_simulator.h"
#include "app/ota_orchestrator.h"
#include "app/platform_orchestrator.h"
#include "app/svt_orchestrator.h"
#include "common/logger.h"
#include "common/types.h"
#include "hal/mock/mock_hals.h"
#include "hal/sa515m/openssl_crypto_hal.h"
#include "hal/sa515m/sqlite_storage_hal.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#define TCU_REQUIRE(expr)                                                                        \
    do {                                                                                         \
        if (!(expr)) {                                                                           \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> " << #expr << "\n";    \
            std::exit(1);                                                                        \
        }                                                                                        \
    } while (0)

namespace tcu::test {

inline bool waitUntil(const std::function<bool()>& pred, int timeout_ms = 3000, int step_ms = 15) {
    const uint64_t deadline = steadyNowMs() + static_cast<uint64_t>(timeout_ms);
    while (steadyNowMs() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return pred();
}

/// Full 6-Daemon + Virtual MCU In-Process Cluster Fixture communicating over real AF_UNIX SEQPACKET sockets.
/// Note: Unix sun_path max is 107 bytes, so keep runtime_dir short!
struct TcuTestCluster {
    std::string               runtime_dir;
    TcuConfig                 cfg;

    // Hardware Simulator & Mock/Concrete HAL instances
    std::unique_ptr<app::McuSimulator>         mcu_sim;
    hal::MockMcuIpcHAL                         mcu_hal;
    hal::MockWatchdogHAL                       mcu_wdg_hal;
    hal::MockModemHAL                          modem_hal;
    hal::MockGnssHAL                           gnss_hal;
    hal::MockAudioHAL                          audio_hal;
    std::unique_ptr<hal::MockCloudHAL>         cloud_hal;
    std::unique_ptr<hal::MockOtaBootHAL>       ota_boot_hal;
    hal::MockWatchdogHAL                       diag_wdg_hal;
    hal::OpenSslCryptoHAL                      crypto_hal;
    hal::SqliteStorageHAL                      platform_storage_hal;
    hal::SqliteStorageHAL                      ota_storage_hal;
    hal::SqliteStorageHAL                      diag_storage_hal;

    // 6 Tier-1 Orchestrators
    std::unique_ptr<app::McuIpcOrchestrator>   mcu_ipcd;
    std::unique_ptr<app::PlatformOrchestrator> platformd;
    std::unique_ptr<app::EcallOrchestrator>    ecalld;
    std::unique_ptr<app::OtaOrchestrator>      otad;
    std::unique_ptr<app::SvtOrchestrator>      svtd;
    std::unique_ptr<app::DiagOrchestrator>     diagd;

    explicit TcuTestCluster(const std::string& tag)
        : gnss_hal(21.028511, 105.804817) {
        common::Logger::init("test-" + tag, "warn");

        runtime_dir = "/tmp/tcu_t_" + tag + "_" + std::to_string(::getpid());
        std::error_code ec;
        std::filesystem::remove_all(runtime_dir, ec);
        std::filesystem::create_directories(runtime_dir, ec);

        cfg.system.hal_mode             = "mock";
        cfg.system.runtime_dir          = runtime_dir;
        cfg.ipc.broker_socket           = runtime_dir + "/ipc.sock";
        cfg.ipc.diag_socket             = runtime_dir + "/diag.sock";
        cfg.mcu_link.mock_socket        = runtime_dir + "/mcu.sock";
        cfg.cloud.spool_dir             = runtime_dir + "/spool";
        cfg.ota.staging_dir             = runtime_dir + "/ota";
        cfg.ota.slot_state_file         = runtime_dir + "/slots.json";
        cfg.ota.public_key_pem          = runtime_dir + "/ota_pub.pem";
        cfg.storage.db_path             = runtime_dir + "/tcu.db";

        // Fast deterministic timers for automated tests
        cfg.ecall.sos_debounce_ms           = 400;
        cfg.svt.zero_speed_hold_ms          = 200;
        cfg.svt.moving_report_interval_ms   = 250;
        cfg.diagnostics.bist_interval_sec   = 60;

        cloud_hal    = std::make_unique<hal::MockCloudHAL>(cfg.cloud.spool_dir);
        ota_boot_hal = std::make_unique<hal::MockOtaBootHAL>(cfg.ota.slot_state_file, "2.3.0");
        mcu_sim      = std::make_unique<app::McuSimulator>(cfg.mcu_link.mock_socket, 60);
    }

    bool startAll() {
        if (!mcu_sim->start()) return false;

        mcu_ipcd = std::make_unique<app::McuIpcOrchestrator>(cfg, mcu_hal, mcu_wdg_hal);
        if (!mcu_ipcd->start()) return false;

        platformd = std::make_unique<app::PlatformOrchestrator>(
            cfg, modem_hal, gnss_hal, audio_hal, *cloud_hal, platform_storage_hal, crypto_hal);
        if (!platformd->start()) return false;

        ecalld = std::make_unique<app::EcallOrchestrator>(cfg);
        if (!ecalld->start()) return false;

        otad = std::make_unique<app::OtaOrchestrator>(
            cfg, *ota_boot_hal, crypto_hal, ota_storage_hal);
        if (!otad->start()) return false;

        svtd = std::make_unique<app::SvtOrchestrator>(cfg, crypto_hal);
        if (!svtd->start()) return false;

        diagd = std::make_unique<app::DiagOrchestrator>(
            cfg, diag_storage_hal, crypto_hal, *ota_boot_hal, diag_wdg_hal);
        if (!diagd->start()) return false;

        // Allow initial pub/sub subscriptions & first vehicle snapshot to settle
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return true;
    }

    void stopAll() {
        if (diagd)     { diagd->stop();     diagd.reset(); }
        if (svtd)      { svtd->stop();      svtd.reset(); }
        if (otad)      { otad->stop();      otad.reset(); }
        if (ecalld)    { ecalld->stop();    ecalld.reset(); }
        if (platformd) { platformd->stop(); platformd.reset(); }
        if (mcu_ipcd)  { mcu_ipcd->stop();  mcu_ipcd.reset(); }
        if (mcu_sim)   { mcu_sim->stop();   mcu_sim.reset(); }
    }

    ~TcuTestCluster() {
        stopAll();
        std::error_code ec;
        std::filesystem::remove_all(runtime_dir, ec);
    }
};

} // namespace tcu::test

