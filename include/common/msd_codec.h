#pragma once

#include "common/types.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace tcu::common {

constexpr size_t kEcallMsdSizeBytes = 140;

struct EcallMsdPayload {
    uint8_t     format_version{2};          // CEN EN 15722 v2/v3
    uint8_t     message_identifier{1};      // Increments on MSD retransmission
    bool        automatic_activation{true}; // true = Crash/Airbag, false = Manual SOS
    bool        test_call{false};           // true = UDS 0x31 / Test Service call
    bool        position_can_be_trusted{true};
    uint8_t     vehicle_type{1};            // 1 = Passenger Car Class M1
    std::string vin{"VN1TCU515M0000001"};   // 17-char ISO 3779 VIN
    uint8_t     propulsion_mask{0x04};      // Bitmask of propulsion energy storage
    uint32_t    timestamp_utc{0};
    int32_t     latitude_mas{0};            // WGS84 milliarcseconds [-324000000..324000000]
    int32_t     longitude_mas{0};           // WGS84 milliarcseconds [-648000000..648000000]
    uint8_t     vehicle_direction{0xFF};    // 0..179 (2-degree increments), 0xFF = unknown
    int16_t     recent_n1_lat_delta{0};     // 100 mas (0.1 arcsec) [-512..+511]
    int16_t     recent_n1_lon_delta{0};
    int16_t     recent_n2_lat_delta{0};
    int16_t     recent_n2_lon_delta{0};
    uint8_t     number_of_passengers{1};
    int16_t     crash_delta_v_kmh{0};
    bool        rollover_detected{false};
};

class MsdCodec {
public:
    static int32_t degreesToMilliArcSeconds(double deg) noexcept;
    static double  milliArcSecondsToDegrees(int32_t mas) noexcept;

    /// Construct EcallMsdPayload from live GNSS trail, VIN, and crash trigger metadata.
    static EcallMsdPayload buildFromTelemetry(
        const std::string& vin,
        uint8_t vehicle_type,
        uint8_t propulsion_mask,
        uint8_t message_id,
        const CrashEvent& crash,
        const GnssTrail& trail);

    /// Deterministically serialize into a 140-byte CEN EN 15722 binary MSD frame.
    static std::array<uint8_t, kEcallMsdSizeBytes> encode(const EcallMsdPayload& msd) noexcept;

    /// Deserialize and verify integrity of a 140-byte MSD frame.
    static std::optional<EcallMsdPayload> decode(
        const std::array<uint8_t, kEcallMsdSizeBytes>& raw) noexcept;
};

} // namespace tcu::common

