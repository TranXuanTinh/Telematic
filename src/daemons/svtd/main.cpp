#include "app/svt_orchestrator.h"
#include "common/config_loader.h"
#include "common/logger.h"
#include "hal/sa515m/openssl_crypto_hal.h"
#include "hal/sa515m/sa515m_hals.h"

#include <atomic>
#include <chrono>
#include <csignal>
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
    if (force_mock) {
        cfg.system.hal_mode = "mock";
    }

    tcu::common::Logger::init("tcu-svtd", cfg.system.log_level);
    tcu::common::Logger::info("Starting tcu-svtd (Stolen Vehicle Tracking & Safe Immobilizer)");

    tcu::hal::OpenSslCryptoHAL crypto_hal;
    tcu::app::SvtOrchestrator orchestrator(cfg, crypto_hal);
    if (!orchestrator.start()) {
        tcu::common::Logger::error("Failed to start SvtOrchestrator");
        return 1;
    }

    tcu::hal::LinuxWatchdogHAL::sendSystemdNotify("READY=1");
    tcu::common::Logger::info("tcu-svtd active");

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tcu::common::Logger::info("Shutting down tcu-svtd...");
    orchestrator.stop();
    return 0;
}

