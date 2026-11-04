#include "app/mcu_simulator.h"
#include "common/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
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

    std::string socket_path = "/tmp/tcu/mcu_spi.sock";
    int period_ms = 100;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && (i + 1) < argc) {
            socket_path = argv[++i];
        } else if (arg == "--period-ms" && (i + 1) < argc) {
            period_ms = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: tcu-mcu-sim [--socket <path>] [--period-ms <ms>]\n";
            return 0;
        }
    }

    std::error_code ec;
    auto parent = std::filesystem::path(socket_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    tcu::common::Logger::init("tcu-mcu-sim", "info");
    tcu::common::Logger::info("Starting Virtual Companion MCU Simulator on " + socket_path);

    tcu::app::McuSimulator sim(socket_path, period_ms);
    if (!sim.start()) {
        tcu::common::Logger::error("Failed to bind Virtual MCU socket: " + socket_path);
        return 1;
    }

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tcu::common::Logger::info("Stopping Virtual Companion MCU Simulator...");
    sim.stop();
    return 0;
}

