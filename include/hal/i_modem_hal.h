#pragma once

#include "common/msd_codec.h"
#include "common/types.h"

#include <array>
#include <functional>
#include <string>

namespace tcu::hal {

/// Pure Virtual HAL for Cellular 4G/5G Modem & VoLTE/CS Emergency Voice.
class IModemHAL {
public:
    using IncomingCallCallback = std::function<void(const std::string& caller_number)>;

    virtual ~IModemHAL() = default;

    virtual bool initialize(const std::string& apn) = 0;
    virtual ModemStatus getStatus() const = 0;

    /// Dial emergency PSAP ("112") or configured non-emergency test number.
    virtual bool dialEmergency(const std::string& dial_number, bool is_test_call) = 0;

    /// Transmit 140-byte CEN EN 15722 MSD and wait for PSAP LL-ACK + HL-ACK.
    virtual bool sendInBandMsd(
        const std::array<uint8_t, common::kEcallMsdSizeBytes>& msd_bytes,
        int timeout_ms) = 0;

    virtual bool answerIncomingCall() = 0;
    virtual bool hangupCall() = 0;

    virtual void setIncomingCallCallback(IncomingCallCallback cb) = 0;
};

} // namespace tcu::hal

