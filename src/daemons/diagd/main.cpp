#include "app/diag_orchestrator.h"
#include "common/config_loader.h"
#include "common/logger.h"
#include "hal/mock/mock_hals.h"
#include "hal/sa515m/openssl_crypto_hal.h"
#include "hal/sa515m/sa515m_hals.h"
#include "hal/sa515m/sqlite_storage_hal.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};

void onSignal(int) {
    g_stop.store(true);
}
} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    bool force_mock = false;
    auto cfg = tcu::common::ConfigLoader::loadFromArgs(argc, argv, &force_mock);
#ifdef TCU_DEFAULT_MOCK_HAL
    const bool use_mock = force_mock || (cfg.system.hal_mode != "sa515m");
#else
    const bool use_mock = force_mock || (cfg.system.hal_mode == "mock");
#endif
    if (use_mock) {
        cfg.system.hal_mode = "mock";
    }

    std::error_code ec;
    std::filesystem::create_directories(cfg.system.runtime_dir, ec);

    tcu::common::Logger::init("tcu-diagd", cfg.system.log_level);
    tcu::common::Logger::info("Starting tcu-diagd (BIST, DTC Store & ISO 14229 UDS Server)");

    std::unique_ptr<tcu::hal::IOtaBootHAL>  ota_boot_hal;
    std::unique_ptr<tcu::hal::IWatchdogHAL> wdg_hal;

    if (use_mock) {
        ota_boot_hal = std::make_unique<tcu::hal::MockOtaBootHAL>(
            cfg.ota.slot_state_file, cfg.system.sw_version);
        wdg_hal = std::make_unique<tcu::hal::MockWatchdogHAL>();
    } else {
        ota_boot_hal = std::make_unique<tcu::hal::RaucOtaBootHAL>();
        wdg_hal      = std::make_unique<tcu::hal::LinuxWatchdogHAL>();
    }

    tcu::hal::SqliteStorageHAL storage_hal;
    tcu::hal::OpenSslCryptoHAL crypto_hal;

    tcu::app::DiagOrchestrator orchestrator(cfg, storage_hal, crypto_hal, *ota_boot_hal, *wdg_hal);
    if (!orchestrator.start()) {
        tcu::common::Logger::error("Failed to start DiagOrchestrator");
        return 1;
    }

    tcu::common::Logger::info("tcu-diagd active");

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tcu::common::Logger::info("Shutting down tcu-diagd...");
    orchestrator.stop();
    return 0;
}

