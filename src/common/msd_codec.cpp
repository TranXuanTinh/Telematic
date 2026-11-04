#include "common/msd_codec.h"
#include "common/crc.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tcu::common {

namespace {

constexpr uint8_t kMsdMagic0 = 0xEC;
constexpr uint8_t kMsdMagic1 = 0x15; // EN 15722 marker

void writeU16BE(uint8_t* dst, uint16_t val) noexcept {
    dst[0] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
    dst[1] = static_cast<uint8_t>(val & 0xFFU);
}

uint16_t readU16BE(const uint8_t* src) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(src[0]) << 8U) | src[1]);
}

void writeU32BE(uint8_t* dst, uint32_t val) noexcept {
    dst[0] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
    dst[1] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
    dst[3] = static_cast<uint8_t>(val & 0xFFU);
}

uint32_t readU32BE(const uint8_t* src) noexcept {
    return (static_cast<uint32_t>(src[0]) << 24U) |
           (static_cast<uint32_t>(src[1]) << 16U) |
           (static_cast<uint32_t>(src[2]) << 8U) |
           static_cast<uint32_t>(src[3]);
}

int16_t clampDelta100Mas(int32_t diff_mas) noexcept {
    int32_t scaled = diff_mas / 100;
    return static_cast<int16_t>(std::clamp<int32_t>(scaled, -512, 511));
}

} // namespace

int32_t MsdCodec::degreesToMilliArcSeconds(double deg) noexcept {
    double raw = std::round(deg * 3600000.0);
    raw = std::clamp(raw, -648000000.0, 648000000.0);
    return static_cast<int32_t>(raw);
}

double MsdCodec::milliArcSecondsToDegrees(int32_t mas) noexcept {
    return static_cast<double>(mas) / 3600000.0;
}

EcallMsdPayload MsdCodec::buildFromTelemetry(
    const std::string& vin,
    uint8_t vehicle_type,
    uint8_t propulsion_mask,
    uint8_t message_id,
    const CrashEvent& crash,
    const GnssTrail& trail) {
    EcallMsdPayload msd;
    msd.format_version = 2;
    msd.message_identifier = (message_id == 0) ? 1 : message_id;
    msd.automatic_activation = crash.automatic_activation;
    msd.test_call = crash.test_call;
    msd.position_can_be_trusted = trail.current.fix_valid && (trail.current.hdop <= 5.0f);
    msd.vehicle_type = vehicle_type;
    msd.vin = vin;
    msd.propulsion_mask = propulsion_mask;
    msd.timestamp_utc = static_cast<uint32_t>(
        crash.timestamp_utc != 0 ? crash.timestamp_utc : currentEpochSeconds());

    msd.latitude_mas = std::clamp<int32_t>(
        degreesToMilliArcSeconds(trail.current.lat_deg), -324000000, 324000000);
    msd.longitude_mas = std::clamp<int32_t>(
        degreesToMilliArcSeconds(trail.current.lon_deg), -648000000, 648000000);

    if (trail.current.fix_valid && trail.current.heading_deg >= 0.0f &&
        trail.current.heading_deg < 360.0f) {
        msd.vehicle_direction = static_cast<uint8_t>(
            std::clamp<int>(static_cast<int>(std::round(trail.current.heading_deg / 2.0f)) % 180, 0, 179));
    } else {
        msd.vehicle_direction = 0xFF;
    }

    if (trail.has_n1) {
        int32_t n1_lat = degreesToMilliArcSeconds(trail.n1.lat_deg);
        int32_t n1_lon = degreesToMilliArcSeconds(trail.n1.lon_deg);
        msd.recent_n1_lat_delta = clampDelta100Mas(n1_lat - msd.latitude_mas);
        msd.recent_n1_lon_delta = clampDelta100Mas(n1_lon - msd.longitude_mas);
    }

    if (trail.has_n2) {
        int32_t n2_lat = degreesToMilliArcSeconds(trail.n2.lat_deg);
        int32_t n2_lon = degreesToMilliArcSeconds(trail.n2.lon_deg);
        msd.recent_n2_lat_delta = clampDelta100Mas(n2_lat - msd.latitude_mas);
        msd.recent_n2_lon_delta = clampDelta100Mas(n2_lon - msd.longitude_mas);
    }

    msd.number_of_passengers = crash.passengers;
    msd.crash_delta_v_kmh = crash.delta_v_x_kmh;
    msd.rollover_detected = crash.rollover;
    return msd;
}

std::array<uint8_t, kEcallMsdSizeBytes> MsdCodec::encode(const EcallMsdPayload& msd) noexcept {
    std::array<uint8_t, kEcallMsdSizeBytes> out{};
    out.fill(0U);

    out[0] = kMsdMagic0;
    out[1] = kMsdMagic1;
    out[2] = msd.format_version;
    out[3] = msd.message_identifier;

    uint8_t control_bits = 0U;
    if (msd.automatic_activation) control_bits |= 0x01U;
    if (msd.test_call)            control_bits |= 0x02U;
    if (msd.position_can_be_trusted) control_bits |= 0x04U;
    if (msd.rollover_detected)    control_bits |= 0x08U;
    control_bits |= static_cast<uint8_t>((msd.vehicle_type & 0x0FU) << 4U);
    out[4] = control_bits;

    // Bytes 5..21: ISO 3779 17-character VIN (padded with '0' if shorter)
    for (size_t i = 0; i < 17; ++i) {
        char c = (i < msd.vin.size()) ? msd.vin[i] : '0';
        // ISO 3779 forbids I, O, Q
        if (c == 'I' || c == 'O' || c == 'Q') {
            c = 'X';
        }
        out[5 + i] = static_cast<uint8_t>(c);
    }

    out[22] = msd.propulsion_mask;
    writeU32BE(&out[23], msd.timestamp_utc);
    writeU32BE(&out[27], static_cast<uint32_t>(msd.latitude_mas));
    writeU32BE(&out[31], static_cast<uint32_t>(msd.longitude_mas));
    out[35] = msd.vehicle_direction;

    writeU16BE(&out[36], static_cast<uint16_t>(msd.recent_n1_lat_delta));
    writeU16BE(&out[38], static_cast<uint16_t>(msd.recent_n1_lon_delta));
    writeU16BE(&out[40], static_cast<uint16_t>(msd.recent_n2_lat_delta));
    writeU16BE(&out[42], static_cast<uint16_t>(msd.recent_n2_lon_delta));

    out[44] = msd.number_of_passengers;
    writeU16BE(&out[45], static_cast<uint16_t>(msd.crash_delta_v_kmh));

    // Bytes 136..139: CRC-32 over bytes [0..135]
    uint32_t crc = computeCrc32(out.data(), kEcallMsdSizeBytes - 4);
    writeU32BE(&out[kEcallMsdSizeBytes - 4], crc);
    return out;
}

std::optional<EcallMsdPayload> MsdCodec::decode(
    const std::array<uint8_t, kEcallMsdSizeBytes>& raw) noexcept {
    if (raw[0] != kMsdMagic0 || raw[1] != kMsdMagic1) {
        return std::nullopt;
    }

    uint32_t expected_crc = computeCrc32(raw.data(), kEcallMsdSizeBytes - 4);
    uint32_t actual_crc = readU32BE(&raw[kEcallMsdSizeBytes - 4]);
    if (expected_crc != actual_crc) {
        return std::nullopt;
    }

    EcallMsdPayload msd;
    msd.format_version = raw[2];
    msd.message_identifier = raw[3];

    uint8_t control_bits = raw[4];
    msd.automatic_activation = (control_bits & 0x01U) != 0U;
    msd.test_call = (control_bits & 0x02U) != 0U;
    msd.position_can_be_trusted = (control_bits & 0x04U) != 0U;
    msd.rollover_detected = (control_bits & 0x08U) != 0U;
    msd.vehicle_type = static_cast<uint8_t>((control_bits >> 4U) & 0x0FU);

    msd.vin.assign(reinterpret_cast<const char*>(&raw[5]), 17);
    msd.propulsion_mask = raw[22];
    msd.timestamp_utc = readU32BE(&raw[23]);
    msd.latitude_mas = static_cast<int32_t>(readU32BE(&raw[27]));
    msd.longitude_mas = static_cast<int32_t>(readU32BE(&raw[31]));
    msd.vehicle_direction = raw[35];

    msd.recent_n1_lat_delta = static_cast<int16_t>(readU16BE(&raw[36]));
    msd.recent_n1_lon_delta = static_cast<int16_t>(readU16BE(&raw[38]));
    msd.recent_n2_lat_delta = static_cast<int16_t>(readU16BE(&raw[40]));
    msd.recent_n2_lon_delta = static_cast<int16_t>(readU16BE(&raw[42]));

    msd.number_of_passengers = raw[44];
    msd.crash_delta_v_kmh = static_cast<int16_t>(readU16BE(&raw[45]));
    return msd;
}

} // namespace tcu::common

