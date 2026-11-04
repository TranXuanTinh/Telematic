#pragma once

#include "common/types.h"
#include "service/mcu_protocol_service.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tcu::app {

/// Virtual Companion MCU Simulator (NXP S32K144 / STM32G4 Behavioral Model).
/// Models Power FSM, CAN Vehicle Telemetry, Crash/Tamper IRQs, Zero-Speed Immobilizer Gate,
/// and Dual-Bank Flash DFU Bootloader over binary CRC-16 SPI/Socket transport.
class McuSimulator {
public:
    explicit McuSimulator(std::string socket_path, int telemetry_period_ms = 100);
    ~McuSimulator();

    bool start();
    void stop();

    // Direct Hardware State Inspection (for verification & HIL assertions)
    VehicleState getVehicleState() const;
    PowerState getPowerState() const;
    EcallLedMode getEcallLedMode() const;
    uint8_t activeFlashBank() const;
    uint32_t activeFirmwareVersion() const;
    uint32_t activeFirmwareCrc32() const;
    uint64_t heartbeatCount() const noexcept { return heartbeats_rx_.load(); }

    // Direct Simulation / Fault Injection Controls
    void setVehicleState(const VehicleState& state);
    void injectCrash(const CrashEvent& crash);
    void injectTamper(uint8_t tamper_flags, float pitch_deg = 0.0f, float roll_deg = 0.0f);
    void requestSleepPreparation(uint16_t grace_ms = 2000);

private:
    void serverLoop();
    void handleSoCFrame(int client_fd, const uint8_t* raw, size_t len);
    void sendToSoCLocked(int client_fd, const service::McuWireFrame& frame);
    void sendTelemetrySnapshotLocked(int client_fd);

    std::string           socket_path_;
    int                   telemetry_period_ms_;
    int                   listen_fd_{-1};
    int                   client_fd_{-1};
    std::atomic<bool>     running_{false};
    std::atomic<uint16_t> next_seq_{1};
    std::atomic<uint64_t> heartbeats_rx_{0};
    std::thread           worker_;

    mutable std::mutex    mtx_;
    PowerState            power_state_{PowerState::ACTIVE_RUN};
    WakeupReason          wakeup_reason_{WakeupReason::KL15_IGN_ON};
    VehicleState          vehicle_{};
    EcallLedMode          led_mode_{EcallLedMode::GREEN_READY};

    // Dual-Bank MCU Flash Bootloader State
    uint8_t               active_bank_{0}; // 0 = Bank A, 1 = Bank B
    uint32_t              active_fw_ver_{0x00010700U}; // v1.7.0
    uint32_t              active_fw_crc32_{0};
    uint32_t              staged_expected_size_{0};
    uint32_t              staged_expected_crc32_{0};
    uint32_t              staged_fw_ver_{0};
    std::vector<uint8_t>  inactive_bank_buf_;
};

} // namespace tcu::app

