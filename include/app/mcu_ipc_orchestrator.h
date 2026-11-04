#pragma once

#include "common/types.h"
#include "hal/i_mcu_ipc_hal.h"
#include "hal/i_watchdog_hal.h"
#include "ipc/ipc_client.h"
#include "ipc/uds_broker.h"
#include "service/mcu_protocol_service.h"
#include "service/power_manager_service.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace tcu::app {

/// Tier 1 Orchestrator for tcu-mcu-ipcd:
/// Hosts Intra-SoC UDS Broker, owns SPI/UART Companion MCU link, coordinates Power & Watchdog.
class McuIpcOrchestrator {
public:
    McuIpcOrchestrator(const TcuConfig& cfg,
                       hal::IMcuIpcHAL& mcu_hal,
                       hal::IWatchdogHAL& watchdog_hal);
    ~McuIpcOrchestrator();

    bool start();
    void stop();

    VehicleState latestVehicleState() const;
    EcallLedMode currentLedMode() const;

private:
    void onMcuFrame(const service::McuWireFrame& frame);
    void registerRpcHandlers();
    void heartbeatLoop();

    TcuConfig                     cfg_;
    hal::IMcuIpcHAL&              mcu_hal_;
    hal::IWatchdogHAL&            watchdog_hal_;

    ipc::UdsBroker                broker_;
    ipc::IpcClient                ipc_;
    service::McuProtocolService   mcu_proto_;
    service::PowerManagerService  power_mgr_;

    std::atomic<bool>             running_{false};
    std::thread                   hb_thread_;

    mutable std::mutex            state_mtx_;
    VehicleState                  vehicle_{};
    EcallLedMode                  led_mode_{EcallLedMode::GREEN_READY};
};

} // namespace tcu::app

