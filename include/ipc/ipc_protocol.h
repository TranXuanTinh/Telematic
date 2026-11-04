#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tcu::ipc {

constexpr uint32_t kIpcMagic          = 0x54435549U; // "TCUI"
constexpr size_t   kMaxIpcPacketBytes = 65536;

enum class DaemonId : uint16_t {
    UNKNOWN   = 0,
    MCU_IPCD  = 1,
    PLATFORMD = 2,
    ECALLD    = 3,
    OTAD      = 4,
    SVTD      = 5,
    DIAGD     = 6,
    TCU_CTL   = 7,
    TEST_NODE = 100
};

enum class IpcMsgKind : uint8_t {
    HELLO_REGISTER  = 1,
    SUBSCRIBE_TOPIC = 2,
    PUBLISH_EVENT   = 3,
    RPC_REQUEST     = 4,
    RPC_RESPONSE    = 5
};

enum IpcFlags : uint8_t {
    IPC_FLAG_NONE   = 0x00,
    IPC_FLAG_URGENT = 0x01  // Preemptive priority queue (used by eCall P0)
};

enum class IpcStatus : uint8_t {
    OK                 = 0,
    ERR_TIMEOUT        = 1,
    ERR_TARGET_OFFLINE = 2,
    ERR_UNSUPPORTED    = 3,
    ERR_PREEMPTED      = 4,
    ERR_REJECTED       = 5,
    ERR_INTERNAL       = 6
};

// Pub/Sub Topic Identifiers (matching HLD Section 3.1)
enum class TopicId : uint16_t {
    TOPIC_VEHICLE_STATE     = 0x0101,
    TOPIC_EMERGENCY_TRIGGER = 0x0102,
    TOPIC_SECURITY_TAMPER   = 0x0103,
    TOPIC_POWER_TRANSITION  = 0x0104,
    TOPIC_GNSS_FIX          = 0x0201,
    TOPIC_CLOUD_COMMAND     = 0x0202,
    TOPIC_INCOMING_CALL     = 0x0203,
    TOPIC_BIST_HEALTH       = 0x0301,
    TOPIC_OTA_PROGRESS      = 0x0401
};

// RPC Method Identifiers (matching HLD Section 3.2)
enum class RpcMethodId : uint16_t {
    // Provided by tcu-platformd
    ACQUIRE_EMERGENCY_CHANNEL = 0x1001,
    RELEASE_EMERGENCY_CHANNEL = 0x1002,
    DIAL_EMERGENCY_AND_MSD    = 0x1003,
    HANGUP_CALL               = 0x1004,
    ANSWER_INCOMING_CALL      = 0x1005,
    SET_VOICE_AUDIO_ROUTE     = 0x1006,
    GET_GNSS_TRAIL            = 0x1007,
    SET_GNSS_RATE             = 0x1008,
    GET_MODEM_STATUS          = 0x1009,
    PUBLISH_CLOUD_TELEMETRY   = 0x100A,
    DOWNLOAD_HTTPS_ASSET      = 0x100B,
    EXECUTE_AUDIO_LOOPBACK    = 0x100C,

    // Provided by tcu-mcu-ipcd
    REQUEST_POWER_LOCK        = 0x2001,
    RELEASE_POWER_LOCK        = 0x2002,
    GET_VEHICLE_STATE         = 0x2003,
    FLASH_MCU_FIRMWARE_BANK   = 0x2004,
    SET_IMMOBILIZER_STATE     = 0x2005,
    SET_ECALL_LED             = 0x2006,
    PING_MCU_LINK             = 0x2007,
    SLEEP_READY_ACK           = 0x2008,

    // Provided by feature daemons & tcu-diagd
    GET_DAEMON_STATUS         = 0x3001,
    RUN_BIST_NOW              = 0x3002,
    PROCESS_UDS_PDU           = 0x3003,
    INJECT_TEST_EVENT         = 0x3004,
    TRIGGER_OTA_CAMPAIGN      = 0x3005,
    COMMIT_POST_BOOT_OTA      = 0x3006
};

#pragma pack(push, 1)
struct IpcHeader {
    uint32_t magic{kIpcMagic};
    uint8_t  kind{0};
    uint8_t  flags{IPC_FLAG_NONE};
    uint8_t  status{static_cast<uint8_t>(IpcStatus::OK)};
    uint8_t  reserved{0};
    uint16_t topic_or_method{0};
    uint16_t sender_id{0};
    uint16_t target_id{0};
    uint32_t corr_id{0};
    uint32_t payload_len{0};
};
#pragma pack(pop)

struct IpcMessage {
    IpcHeader            header{};
    std::string          payload;

    std::vector<uint8_t> serialize() const {
        IpcHeader h = header;
        h.magic = kIpcMagic;
        h.payload_len = static_cast<uint32_t>(payload.size());
        std::vector<uint8_t> buf(sizeof(IpcHeader) + payload.size());
        std::memcpy(buf.data(), &h, sizeof(IpcHeader));
        if (!payload.empty()) {
            std::memcpy(buf.data() + sizeof(IpcHeader), payload.data(), payload.size());
        }
        return buf;
    }

    static bool deserialize(const uint8_t* raw, size_t len, IpcMessage& out) {
        if (!raw || len < sizeof(IpcHeader)) {
            return false;
        }
        std::memcpy(&out.header, raw, sizeof(IpcHeader));
        if (out.header.magic != kIpcMagic) {
            return false;
        }
        if (sizeof(IpcHeader) + out.header.payload_len > len) {
            return false;
        }
        out.payload.assign(
            reinterpret_cast<const char*>(raw + sizeof(IpcHeader)),
            out.header.payload_len);
        return true;
    }
};

} // namespace tcu::ipc

