#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tcu::hal {

struct McuLinkOpenParams {
    std::string device_or_socket;
    uint32_t    speed_hz_or_baud{10000000};
    bool        use_mock_socket{false};
};

/// Pure Virtual HAL for SA515M <-> Companion MCU Physical Transport (SPI / UART).
class IMcuIpcHAL {
public:
    virtual ~IMcuIpcHAL() = default;

    virtual bool open(const McuLinkOpenParams& params) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    /// Transmit a binary frame to Companion MCU.
    virtual bool sendFrame(const uint8_t* data, size_t len) = 0;

    /// Poll and read next raw binary frame from Companion MCU within timeout_ms.
    /// Returns number of bytes read (>0), 0 on timeout, or <0 on link error.
    virtual int receiveFrame(std::vector<uint8_t>& out_frame, int timeout_ms) = 0;
};

} // namespace tcu::hal

