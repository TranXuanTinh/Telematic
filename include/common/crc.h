#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tcu::common {

/// CRC-16-CCITT (polynomial 0x1021, initial value 0xFFFF, no final XOR)
/// Used on all binary SPI/UART frames between SA515M and Companion MCU.
uint16_t computeCrc16Ccitt(const uint8_t* data, size_t length) noexcept;

inline uint16_t computeCrc16Ccitt(const std::vector<uint8_t>& buf) noexcept {
    return computeCrc16Ccitt(buf.data(), buf.size());
}

/// IEEE 802.3 CRC-32 (reflected polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF)
/// Used to verify Companion MCU Dual-Bank Flash firmware images and MSD integrity.
uint32_t computeCrc32(const uint8_t* data, size_t length) noexcept;

inline uint32_t computeCrc32(const std::vector<uint8_t>& buf) noexcept {
    return computeCrc32(buf.data(), buf.size());
}

} // namespace tcu::common

