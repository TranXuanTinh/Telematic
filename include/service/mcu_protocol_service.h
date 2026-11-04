#pragma once

#include "common/types.h"
#include "hal/i_mcu_ipc_hal.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tcu::service {

// LLD Section 1.2: Inter-Processor Binary Frame Constants
constexpr uint8_t kMcuSof0 = 0x55;
constexpr uint8_t kMcuSof1 = 0xAA;

enum McuFrameFlags : uint8_t {
    MCU_FLAG_NONE        = 0x00,
    MCU_FLAG_ACK_REQ     = 0x01,
    MCU_FLAG_IS_ACK      = 0x02,
    MCU_FLAG_IS_NACK     = 0x04,
    MCU_FLAG_URGENT_WAKE = 0x08
};

enum class McuMsgId : uint8_t {
    MSG_HEARTBEAT_WDG     = 0x01,
    MSG_WAKEUP_REASON     = 0x02,
    MSG_POWER_TRANSITION  = 0x03,
    MSG_VEHICLE_TELEMETRY = 0x10,
    MSG_CRASH_EVENT       = 0x11,
    MSG_TAMPER_EVENT      = 0x12,
    MSG_SET_ECALL_LED     = 0x20,
    MSG_SET_IMMOBILIZER   = 0x21,
    MSG_UDS_ISOTP_PDU     = 0x30,
    MSG_MCU_OTA_BEGIN     = 0x40,
    MSG_MCU_OTA_CHUNK     = 0x41,
    MSG_MCU_OTA_COMMIT    = 0x42,
    MSG_SIM_INJECT_CMD    = 0xF0 // Simulation/HIL control frame for tcu-mcu-sim
};

struct McuWireFrame {
    McuMsgId             msg_id{McuMsgId::MSG_HEARTBEAT_WDG};
    uint8_t              flags{MCU_FLAG_NONE};
    uint16_t             seq_num{0};
    std::vector<uint8_t> payload;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* raw, size_t len, McuWireFrame& out, bool* crc_error = nullptr);
};

#pragma pack(push, 1)
struct PackedVehicleTelemetry {
    uint16_t speed_cms{0};       // 0.01 km/h units (e.g., 8050 = 80.50 km/h)
    uint8_t  ignition_kl15{0};
    uint8_t  gear{0};
    uint8_t  doors_locked{1};
    uint8_t  authorized_driver{0};
    uint16_t kl30_mv{12600};
    uint16_t bub_mv{4100};
    uint8_t  bub_soc{95};
    uint8_t  bub_soh{98};
    uint8_t  starter_inhibit{0};
};

struct PackedCrashEvent {
    uint8_t airbag_mask{0x01};
    int16_t dv_x_kmh{-48};
    int16_t dv_y_kmh{0};
    uint8_t rollover{0};
    uint8_t automatic_activation{1};
};

struct PackedTamperEvent {
    uint8_t tamper_flags{0};
    int16_t tilt_pitch_cdeg{0}; // 0.01 degrees
    int16_t tilt_roll_cdeg{0};
};
#pragma pack(pop)

/// Tier 2 Service managing CRC-16-CCITT binary framing, ACK/Retry, and Dual-Bank MCU DFU.
class McuProtocolService {
public:
    using FrameCallback = std::function<void(const McuWireFrame&)>;

    McuProtocolService(hal::IMcuIpcHAL& hal, int ack_timeout_ms = 50, int max_retries = 3);
    ~McuProtocolService();

    bool start();
    void stop();

    void setFrameCallback(FrameCallback cb);

    /// Send frame without waiting for ACK.
    bool sendUnreliable(McuMsgId msg_id,
                        const std::vector<uint8_t>& payload,
                        uint8_t extra_flags = MCU_FLAG_NONE);

    /// Send frame with ACK_REQ and retry up to max_retries_ on timeout/NACK.
    bool sendReliable(McuMsgId msg_id,
                      const std::vector<uint8_t>& payload,
                      std::vector<uint8_t>* ack_payload_out = nullptr,
                      int custom_timeout_ms = -1);

    /// Stream full Companion MCU firmware image in 512-byte CRC-protected chunks to inactive bank.
    bool flashMcuFirmware(const std::vector<uint8_t>& fw_image, uint32_t fw_version_code);

    uint32_t crcErrorCount() const noexcept { return crc_errors_.load(); }
    uint32_t timeoutCount() const noexcept { return timeouts_.load(); }
    bool isLinkHealthy() const noexcept { return consecutive_failures_.load() < 3; }

private:
    struct PendingAck {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    resolved{false};
        bool                    is_ack{false};
        std::vector<uint8_t>    payload;
    };

    void rxLoop();

    hal::IMcuIpcHAL&                                   hal_;
    int                                                ack_timeout_ms_;
    int                                                max_retries_;
    std::atomic<bool>                                  running_{false};
    std::atomic<uint16_t>                              next_seq_{1};
    std::atomic<uint32_t>                              crc_errors_{0};
    std::atomic<uint32_t>                              timeouts_{0};
    std::atomic<uint32_t>                              consecutive_failures_{0};

    std::thread                                        rx_thread_;
    std::mutex                                         cb_mtx_;
    FrameCallback                                      frame_cb_;

    std::mutex                                         pending_mtx_;
    std::map<uint16_t, std::shared_ptr<PendingAck>>    pending_acks_;
};

} // namespace tcu::service

