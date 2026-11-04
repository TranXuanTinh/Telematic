#include "app/platform_orchestrator.h"
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
    std::filesystem::create_directories(cfg.cloud.spool_dir, ec);

    tcu::common::Logger::init("tcu-platformd", cfg.system.log_level);
    tcu::common::Logger::info("Starting tcu-platformd (mode=" + cfg.system.hal_mode + ")");

    std::unique_ptr<tcu::hal::IModemHAL> modem_hal;
    std::unique_ptr<tcu::hal::IGnssHAL>  gnss_hal;
    std::unique_ptr<tcu::hal::IAudioHAL> audio_hal;
    std::unique_ptr<tcu::hal::ICloudHAL> cloud_hal;

    if (use_mock) {
        modem_hal = std::make_unique<tcu::hal::MockModemHAL>();
        gnss_hal  = std::make_unique<tcu::hal::MockGnssHAL>(
            cfg.gnss.default_lat, cfg.gnss.default_lon);
        audio_hal = std::make_unique<tcu::hal::MockAudioHAL>();
        cloud_hal = std::make_unique<tcu::hal::MockCloudHAL>(cfg.cloud.spool_dir);
    } else {
        modem_hal = std::make_unique<tcu::hal::Sa515mModemHAL>();
        gnss_hal  = std::make_unique<tcu::hal::Sa515mGnssHAL>();
        audio_hal = std::make_unique<tcu::hal::Sa515mAudioHAL>();
        cloud_hal = std::make_unique<tcu::hal::CurlMqttCloudHAL>(
            cfg.cloud.mqtt_broker, cfg.cloud.spool_dir);
    }

    tcu::hal::SqliteStorageHAL storage_hal;
    tcu::hal::OpenSslCryptoHAL crypto_hal;

    tcu::app::PlatformOrchestrator orchestrator(
        cfg, *modem_hal, *gnss_hal, *audio_hal, *cloud_hal, storage_hal, crypto_hal);
    if (!orchestrator.start()) {
        tcu::common::Logger::error("Failed to start PlatformOrchestrator (is tcu-mcu-ipcd running?)");
        return 1;
    }

    tcu::hal::LinuxWatchdogHAL::sendSystemdNotify("READY=1");
    tcu::common::Logger::info("tcu-platformd active");

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tcu::common::Logger::info("Shutting down tcu-platformd...");
    orchestrator.stop();
    return 0;
}

