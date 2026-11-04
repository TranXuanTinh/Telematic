#include "common/crc.h"

namespace tcu::common {

uint16_t computeCrc16Ccitt(const uint8_t* data, size_t length) noexcept {
    uint16_t crc = 0xFFFFU;
    if (!data) {
        return crc;
    }
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<uint16_t>((crc << 1U) ^ 0x1021U);
            } else {
                crc = static_cast<uint16_t>(crc << 1U);
            }
        }
    }
    return crc;
}

uint32_t computeCrc32(const uint8_t* data, size_t length) noexcept {
    uint32_t crc = 0xFFFFFFFFU;
    if (!data) {
        return ~crc;
    }
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

} // namespace tcu::common

