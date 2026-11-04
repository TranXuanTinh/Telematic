#include "app/ecall_orchestrator.h"
#include "common/config_loader.h"
#include "common/logger.h"
#include "hal/sa515m/sa515m_hals.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <pthread.h>
#include <sched.h>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};

void onSignal(int) {
    g_stop.store(true);
}

void tryEnableRealTimeScheduling() {
    sched_param sp{};
    sp.sched_priority = 80;
    if (::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &sp) == 0) {
        tcu::common::Logger::info("Elevated tcu-ecalld to SCHED_FIFO priority 80");
    }
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

    tcu::common::Logger::init("tcu-ecalld", cfg.system.log_level);
    tryEnableRealTimeScheduling();

    tcu::app::EcallOrchestrator orchestrator(cfg);
    if (!orchestrator.start()) {
        tcu::common::Logger::error("Failed to start EcallOrchestrator");
        return 1;
    }

    tcu::hal::LinuxWatchdogHAL::sendSystemdNotify("READY=1");
    tcu::common::Logger::info("tcu-ecalld armed and monitoring emergency triggers (EN 15722 / R144)");

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tcu::common::Logger::info("Shutting down tcu-ecalld...");
    orchestrator.stop();
    return 0;
}

