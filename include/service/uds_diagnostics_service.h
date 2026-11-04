#pragma once

#include "common/types.h"
#include "hal/i_ota_boot_hal.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace tcu::service {

enum class UdsSession : uint8_t {
    DEFAULT_SESSION  = 0x01,
    EXTENDED_SESSION = 0x03,
    SAFETY_EOL       = 0x60
};

struct UdsContextSnapshot {
    std::string             vin;
    std::string             sw_version;
    GnssFix                 gnss;
    ModemStatus             modem;
    VehicleState            vehicle;
    hal::DualBankSlotStatus ota_slots;
    SvtState                svt_state{SvtState::DISARMED};
};

/// ISO 14229-1 Unified Diagnostic Services (UDS) Protocol Server Engine.
/// Implements LLD Section 5.1 Services: 0x10, 0x3E, 0x22, 0x2E, 0x19, 0x14, 0x31.
class UdsDiagnosticsService {
public:
    using ContextProvider     = std::function<UdsContextSnapshot()>;
    using ReadDtcsCallback    = std::function<std::vector<DtcRecord>(uint8_t mask)>;
    using ClearDtcsCallback   = std::function<bool()>;
    using WriteVinCallback    = std::function<bool(const std::string& new_vin)>;
    using RoutineStartHandler = std::function<bool(uint16_t routine_id, std::vector<uint8_t>& out_info)>;

    UdsDiagnosticsService(ContextProvider ctx_provider,
                          ReadDtcsCallback read_dtcs,
                          ClearDtcsCallback clear_dtcs,
                          WriteVinCallback write_vin,
                          RoutineStartHandler routine_handler);

    std::vector<uint8_t> handleRequest(const std::vector<uint8_t>& req);
    UdsSession activeSession() const;

    /// Convert DTC string e.g. "B1A01-13" or "U0100-87" into 3-byte ISO 15031-6 / 14229 DTC code.
    static std::array<uint8_t, 3> encodeIsoDtcBytes(const std::string& dtc_str) noexcept;

private:
    static std::vector<uint8_t> negativeResponse(uint8_t sid, uint8_t nrc);

    mutable std::mutex  mtx_;
    UdsSession          session_{UdsSession::DEFAULT_SESSION};
    uint64_t            last_activity_ms_{0};

    ContextProvider     ctx_provider_;
    ReadDtcsCallback    read_dtcs_;
    ClearDtcsCallback   clear_dtcs_;
    WriteVinCallback    write_vin_;
    RoutineStartHandler routine_handler_;
};

} // namespace tcu::service

