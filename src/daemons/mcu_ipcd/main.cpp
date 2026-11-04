#include "app/mcu_ipc_orchestrator.h"
#include "common/config_loader.h"
#include "common/logger.h"
#include "hal/mock/mock_hals.h"
#include "hal/sa515m/sa515m_hals.h"

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

    tcu::common::Logger::init("tcu-mcu-ipcd", cfg.system.log_level);
    tcu::common::Logger::info("Starting tcu-mcu-ipcd (mode=" + cfg.system.hal_mode +
                              ", broker=" + cfg.ipc.broker_socket + ")");

    std::unique_ptr<tcu::hal::IMcuIpcHAL> mcu_hal;
    std::unique_ptr<tcu::hal::IWatchdogHAL> wdg_hal;

    if (use_mock) {
        mcu_hal = std::make_unique<tcu::hal::MockMcuIpcHAL>();
        wdg_hal = std::make_unique<tcu::hal::MockWatchdogHAL>();
    } else {
        mcu_hal = std::make_unique<tcu::hal::Sa515mMcuIpcHAL>();
        wdg_hal = std::make_unique<tcu::hal::LinuxWatchdogHAL>();
    }

    tcu::app::McuIpcOrchestrator orchestrator(cfg, *mcu_hal, *wdg_hal);
    if (!orchestrator.start()) {
        tcu::common::Logger::error("Failed to start McuIpcOrchestrator");
        return 1;
    }

    tcu::hal::LinuxWatchdogHAL::sendSystemdNotify("READY=1");
    tcu::common::Logger::info("tcu-mcu-ipcd active and serving Intra-SoC UDS Broker");

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tcu::common::Logger::info("Shutting down tcu-mcu-ipcd...");
    orchestrator.stop();
    return 0;
}

