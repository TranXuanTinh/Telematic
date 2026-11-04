#include "service/uds_diagnostics_service.h"
#include "common/json_mini.h"

#include <cstdlib>
#include <cstring>

namespace tcu::service {

namespace {
constexpr uint8_t NRC_SERVICE_NOT_SUPPORTED            = 0x11;
constexpr uint8_t NRC_SUBFUNCTION_NOT_SUPPORTED        = 0x12;
constexpr uint8_t NRC_INCORRECT_MESSAGE_LEN            = 0x13;
constexpr uint8_t NRC_CONDITIONS_NOT_CORRECT           = 0x22;
constexpr uint8_t NRC_REQUEST_OUT_OF_RANGE             = 0x31;
constexpr uint8_t NRC_SECURITY_ACCESS_DENIED           = 0x33;
constexpr uint8_t NRC_SUBFUNC_NOT_SUPPORTED_IN_SESSION = 0x7E;
} // namespace

UdsDiagnosticsService::UdsDiagnosticsService(ContextProvider ctx_provider,
                                             ReadDtcsCallback read_dtcs,
                                             ClearDtcsCallback clear_dtcs,
                                             WriteVinCallback write_vin,
                                             RoutineStartHandler routine_handler)
    : ctx_provider_(std::move(ctx_provider)),
      read_dtcs_(std::move(read_dtcs)),
      clear_dtcs_(std::move(clear_dtcs)),
      write_vin_(std::move(write_vin)),
      routine_handler_(std::move(routine_handler)) {
    last_activity_ms_ = steadyNowMs();
}

UdsSession UdsDiagnosticsService::activeSession() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return session_;
}

std::vector<uint8_t> UdsDiagnosticsService::negativeResponse(uint8_t sid, uint8_t nrc) {
    return {0x7FU, sid, nrc};
}

std::array<uint8_t, 3> UdsDiagnosticsService::encodeIsoDtcBytes(
    const std::string& dtc_str) noexcept {
    // Example: "B1A01-13" -> Prefix 'B' (Body = 0b10), '1A01' -> high/mid, '13' -> FT byte
    std::array<uint8_t, 3> out{0, 0, 0};
    if (dtc_str.size() < 5) {
        return out;
    }

    uint8_t prefix_bits = 0;
    switch (dtc_str[0]) {
        case 'P': prefix_bits = 0x00U; break; // Powertrain
        case 'C': prefix_bits = 0x40U; break; // Chassis
        case 'B': prefix_bits = 0x80U; break; // Body
        case 'U': prefix_bits = 0xC0U; break; // Network
        default:  prefix_bits = 0x80U; break;
    }

    std::string hex_core = dtc_str.substr(1, 4);
    uint16_t core_val = static_cast<uint16_t>(std::strtoul(hex_core.c_str(), nullptr, 16));
    out[0] = static_cast<uint8_t>(prefix_bits | ((core_val >> 8U) & 0x3FU));
    out[1] = static_cast<uint8_t>(core_val & 0xFFU);

    auto dash = dtc_str.find('-');
    if (dash != std::string::npos && dash + 2 < dtc_str.size()) {
        std::string ft = dtc_str.substr(dash + 1, 2);
        out[2] = static_cast<uint8_t>(std::strtoul(ft.c_str(), nullptr, 16));
    }
    return out;
}

std::vector<uint8_t> UdsDiagnosticsService::handleRequest(const std::vector<uint8_t>& req) {
    if (req.empty()) {
        return negativeResponse(0x00, NRC_INCORRECT_MESSAGE_LEN);
    }

    std::lock_guard<std::mutex> lk(mtx_);
    // S3 session timeout check (5000 ms resets non-default session back to Default)
    const uint64_t now_ms = steadyNowMs();
    if (session_ != UdsSession::DEFAULT_SESSION && (now_ms - last_activity_ms_) > 5000U) {
        session_ = UdsSession::DEFAULT_SESSION;
    }
    last_activity_ms_ = now_ms;

    const uint8_t sid = req[0];

    switch (sid) {
        // 0x10 DiagnosticSessionControl
        case 0x10: {
            if (req.size() != 2) return negativeResponse(sid, NRC_INCORRECT_MESSAGE_LEN);
            const uint8_t sub = req[1] & 0x7FU;
            if (sub == 0x01 || sub == 0x03 || sub == 0x60) {
                session_ = static_cast<UdsSession>(sub);
                // P2_Server_Max = 50ms (0x0032), P2*_Server_Max = 5000ms/10ms = 500 (0x01F4)
                return {0x50, sub, 0x00, 0x32, 0x01, 0xF4};
            }
            return negativeResponse(sid, NRC_SUBFUNCTION_NOT_SUPPORTED);
        }

        // 0x3E TesterPresent
        case 0x3E: {
            if (req.size() != 2) return negativeResponse(sid, NRC_INCORRECT_MESSAGE_LEN);
            if ((req[1] & 0x7FU) != 0x00) return negativeResponse(sid, NRC_SUBFUNCTION_NOT_SUPPORTED);
            return {0x7E, 0x00};
        }

        // 0x22 ReadDataByIdentifier
        case 0x22: {
            if (req.size() != 3) return negativeResponse(sid, NRC_INCORRECT_MESSAGE_LEN);
            const uint16_t did = static_cast<uint16_t>((req[1] << 8U) | req[2]);
            const auto snap = ctx_provider_ ? ctx_provider_() : UdsContextSnapshot{};

            std::vector<uint8_t> resp = {0x62, req[1], req[2]};
            switch (did) {
                case 0xF190: { // VIN (17 ASCII bytes)
                    std::string v = snap.vin;
                    v.resize(17, '0');
                    resp.insert(resp.end(), v.begin(), v.end());
                    return resp;
                }
                case 0xF189: { // TCU SW Version
                    resp.insert(resp.end(), snap.sw_version.begin(), snap.sw_version.end());
                    return resp;
                }
                case 0xFD01: { // Live GNSS JSON summary
                    common::JsonValue j = common::JsonValue::object();
                    j["lat"]     = snap.gnss.lat_deg;
                    j["lon"]     = snap.gnss.lon_deg;
                    j["sats"]    = static_cast<int>(snap.gnss.satellites);
                    j["hdop"]    = static_cast<double>(snap.gnss.hdop);
                    j["ant_ok"]  = (snap.gnss.antenna_health == AntennaHealth::OK);
                    std::string s = j.dump(false);
                    resp.insert(resp.end(), s.begin(), s.end());
                    return resp;
                }
                case 0xFD02: { // Modem status
                    common::JsonValue j = common::JsonValue::object();
                    j["rat"]  = snap.modem.rat;
                    j["rssi"] = static_cast<int>(snap.modem.rssi_dbm);
                    j["reg"]  = snap.modem.registered;
                    j["imei"] = snap.modem.imei;
                    std::string s = j.dump(false);
                    resp.insert(resp.end(), s.begin(), s.end());
                    return resp;
                }
                case 0xFD03: { // Power & Backup Battery (KL30 mV, BUB mV, SoC, SoH)
                    resp.push_back(static_cast<uint8_t>((snap.vehicle.kl30_mv >> 8U) & 0xFFU));
                    resp.push_back(static_cast<uint8_t>(snap.vehicle.kl30_mv & 0xFFU));
                    resp.push_back(static_cast<uint8_t>((snap.vehicle.bub_mv >> 8U) & 0xFFU));
                    resp.push_back(static_cast<uint8_t>(snap.vehicle.bub_mv & 0xFFU));
                    resp.push_back(snap.vehicle.bub_soc);
                    resp.push_back(snap.vehicle.bub_soh);
                    return resp;
                }
                case 0xFD04: { // Dual-Bank OTA Status
                    common::JsonValue j = common::JsonValue::object();
                    j["active_slot"]    = snap.ota_slots.active_slot;
                    j["installed_ver"]  = snap.ota_slots.installed_version;
                    j["marked_good"]    = snap.ota_slots.active_marked_good;
                    j["rollback_count"] = snap.ota_slots.rollback_count;
                    std::string s = j.dump(false);
                    resp.insert(resp.end(), s.begin(), s.end());
                    return resp;
                }
                case 0xFD05: { // SVT Security State (Requires Extended / EOL Session)
                    if (session_ == UdsSession::DEFAULT_SESSION) {
                        return negativeResponse(sid, NRC_SECURITY_ACCESS_DENIED);
                    }
                    resp.push_back(static_cast<uint8_t>(snap.svt_state));
                    resp.push_back(snap.vehicle.starter_inhibit_active ? 1U : 0U);
                    return resp;
                }
                default:
                    return negativeResponse(sid, NRC_REQUEST_OUT_OF_RANGE);
            }
        }

        // 0x2E WriteDataByIdentifier (VIN 0xF190 writable only in SAFETY_EOL 0x60)
        case 0x2E: {
            if (req.size() < 4) return negativeResponse(sid, NRC_INCORRECT_MESSAGE_LEN);
            const uint16_t did = static_cast<uint16_t>((req[1] << 8U) | req[2]);
            if (did != 0xF190) return negativeResponse(sid, NRC_REQUEST_OUT_OF_RANGE);
            if (session_ != UdsSession::SAFETY_EOL) {
                return negativeResponse(sid, NRC_SECURITY_ACCESS_DENIED);
            }
            if (req.size() != 3 + 17) return negativeResponse(sid, NRC_INCORRECT_MESSAGE_LEN);
            std::string new_vin(reinterpret_cast<const char*>(&req[3]), 17);
            if (write_vin_ && write_vin_(new_vin)) {
                return {0x6E, req[1], req[2]};
            }
            return negativeResponse(sid, NRC_CONDITIONS_NOT_CORRECT);
        }

        // 0x19 ReadDTCInformation (sub-function 0x02 reportDTCByStatusMask)
        case 0x19: {
            if (req.size() != 3) return negativeResponse(sid, NRC_INCORRECT_MESSAGE_LEN);
            if (req[1] != 0x02) return negativeResponse(sid, NRC_SUBFUNCTION_NOT_SUPPORTED);
            const uint8_t mask = req[2];
            std::vector<uint8_t> resp = {0x59, 0x02, 0x09}; // availabilityStatusMask = 0x09
            if (read_dtcs_) {
                auto dtcs = read_dtcs_(mask);
                for (const auto& d : dtcs) {
                    auto code3 = encodeIsoDtcBytes(d.dtc_code);
                    resp.push_back(code3[0]);
                    resp.push_back(code3[1]);
                    resp.push_back(code3[2]);
                    resp.push_back(d.status_mask);
                }
            }
            return resp;
        }

        // 0x14 ClearDiagnosticInformation (0xFFFFFF = All Groups)
        case 0x14: {
            if (req.size() != 4) return negativeResponse(sid, NRC_INCORRECT_MESSAGE_LEN);
            if (session_ == UdsSession::DEFAULT_SESSION) {
                return negativeResponse(sid, NRC_SUBFUNC_NOT_SUPPORTED_IN_SESSION);
            }
            if (req[1] == 0xFF && req[2] == 0xFF && req[3] == 0xFF) {
                if (clear_dtcs_ && clear_dtcs_()) {
                    return {0x54};
                }
            }
            return negativeResponse(sid, NRC_REQUEST_OUT_OF_RANGE);
        }

        // 0x31 RoutineControl (0x01 startRoutine: 0x0201 Test eCall, 0x0202 Audio Loopback, 0x0203 BIST)
        case 0x31: {
            if (req.size() < 4) return negativeResponse(sid, NRC_INCORRECT_MESSAGE_LEN);
            const uint8_t sub = req[1];
            if (sub != 0x01) return negativeResponse(sid, NRC_SUBFUNCTION_NOT_SUPPORTED);

            const uint16_t rid = static_cast<uint16_t>((req[2] << 8U) | req[3]);
            if ((rid == 0x0201 || rid == 0x0202) && session_ == UdsSession::DEFAULT_SESSION) {
                return negativeResponse(sid, NRC_SUBFUNC_NOT_SUPPORTED_IN_SESSION);
            }
            if (rid != 0x0201 && rid != 0x0202 && rid != 0x0203) {
                return negativeResponse(sid, NRC_REQUEST_OUT_OF_RANGE);
            }

            std::vector<uint8_t> routine_info;
            if (routine_handler_ && routine_handler_(rid, routine_info)) {
                std::vector<uint8_t> resp = {0x71, sub, req[2], req[3]};
                resp.insert(resp.end(), routine_info.begin(), routine_info.end());
                return resp;
            }
            return negativeResponse(sid, NRC_CONDITIONS_NOT_CORRECT);
        }

        default:
            return negativeResponse(sid, NRC_SERVICE_NOT_SUPPORTED);
    }
}

} // namespace tcu::service

