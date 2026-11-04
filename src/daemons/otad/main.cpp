#include "app/ota_orchestrator.h"
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
    std::filesystem::create_directories(cfg.ota.staging_dir, ec);

    tcu::common::Logger::init("tcu-otad", cfg.system.log_level);
    tcu::common::Logger::info("Starting tcu-otad (UN ECE R156 Dual-Domain OTA Manager)");

    std::unique_ptr<tcu::hal::IOtaBootHAL> ota_boot_hal;
    if (use_mock) {
        ota_boot_hal = std::make_unique<tcu::hal::MockOtaBootHAL>(
            cfg.ota.slot_state_file, cfg.system.sw_version);
    } else {
        ota_boot_hal = std::make_unique<tcu::hal::RaucOtaBootHAL>();
    }

    tcu::hal::OpenSslCryptoHAL crypto_hal;
    tcu::hal::SqliteStorageHAL storage_hal;

    tcu::app::OtaOrchestrator orchestrator(cfg, *ota_boot_hal, crypto_hal, storage_hal);
    if (!orchestrator.start()) {
        tcu::common::Logger::error("Failed to start OtaOrchestrator");
        return 1;
    }

    // If booted into an unverified new slot, execute post-boot BIST check automatically
    if (orchestrator.currentState() == tcu::OtaState::POST_BOOT_VERIFY) {
        tcu::common::Logger::info("Pending post-boot OTA slot detected; running BIST verification...");
        orchestrator.evaluatePostBootVerification();
    }

    tcu::hal::LinuxWatchdogHAL::sendSystemdNotify("READY=1");
    tcu::common::Logger::info("tcu-otad active");

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tcu::common::Logger::info("Shutting down tcu-otad...");
    orchestrator.stop();
    return 0;
}

