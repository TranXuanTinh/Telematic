#include "common/crc.h"
#include "common/geo_math.h"
#include "common/json_mini.h"
#include "common/msd_codec.h"
#include "hal/sa515m/openssl_crypto_hal.h"
#include "hal/sa515m/sa515m_hals.h"
#include "test_cluster_harness.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>

int main() {
    using namespace tcu;

    // 1. CRC-16-CCITT ("123456789" -> 0x29B1) & CRC-32 ("123456789" -> 0xCBF43926)
    const char* vec = "123456789";
    uint16_t crc16 = common::computeCrc16Ccitt(reinterpret_cast<const uint8_t*>(vec), 9);
    uint32_t crc32 = common::computeCrc32(reinterpret_cast<const uint8_t*>(vec), 9);
    TCU_REQUIRE(crc16 == 0x29B1U);
    TCU_REQUIRE(crc32 == 0xCBF43926U);

    // 2. CEN EN 15722 140-byte MSD Codec Roundtrip + Corruption Detection
    CrashEvent crash;
    crash.automatic_activation = true;
    crash.test_call            = false;
    crash.delta_v_x_kmh        = -54;
    crash.rollover             = true;
    crash.passengers           = 3;
    crash.timestamp_utc        = 1793754000U;

    GnssTrail trail;
    trail.current.lat_deg     = 21.028511;
    trail.current.lon_deg     = 105.804817;
    trail.current.heading_deg = 180.0f;
    trail.current.fix_valid   = true;
    trail.has_n1              = true;
    trail.n1.lat_deg          = 21.028000;
    trail.n1.lon_deg          = 105.804500;
    trail.has_n2              = true;
    trail.n2.lat_deg          = 21.027500;
    trail.n2.lon_deg          = 105.804000;

    auto msd_in  = common::MsdCodec::buildFromTelemetry("VN1TCU515M0000099", 1, 0x04, 2, crash, trail);
    auto msd_raw = common::MsdCodec::encode(msd_in);
    TCU_REQUIRE(msd_raw.size() == 140U);

    auto msd_out = common::MsdCodec::decode(msd_raw);
    TCU_REQUIRE(msd_out.has_value());
    TCU_REQUIRE(msd_out->vin == "VN1TCU515M0000099");
    TCU_REQUIRE(msd_out->message_identifier == 2U);
    TCU_REQUIRE(msd_out->automatic_activation == true);
    TCU_REQUIRE(msd_out->test_call == false);
    TCU_REQUIRE(msd_out->rollover_detected == true);
    TCU_REQUIRE(msd_out->crash_delta_v_kmh == -54);
    TCU_REQUIRE(msd_out->number_of_passengers == 3U);

    double decoded_lat = common::MsdCodec::milliArcSecondsToDegrees(msd_out->latitude_mas);
    double decoded_lon = common::MsdCodec::milliArcSecondsToDegrees(msd_out->longitude_mas);
    TCU_REQUIRE(std::fabs(decoded_lat - 21.028511) < 1e-5);
    TCU_REQUIRE(std::fabs(decoded_lon - 105.804817) < 1e-5);

    // Flipping 1 bit in MSD frame must fail CRC-16 integrity validation
    auto corrupted_msd = msd_raw;
    corrupted_msd[25] ^= 0x01U;
    TCU_REQUIRE(!common::MsdCodec::decode(corrupted_msd).has_value());

    // 3. Haversine Geofence, Ray-Casting Polygon & SemVer Anti-Rollback Math
    double d_m = common::haversineDistanceMeters(21.028511, 105.804817, 21.030511, 105.804817);
    TCU_REQUIRE(d_m > 215.0 && d_m < 230.0);

    std::vector<std::pair<double, double>> poly = {
        {21.020, 105.800}, {21.030, 105.800}, {21.030, 105.810}, {21.020, 105.810}};
    TCU_REQUIRE(common::isPointInsidePolygon(21.025, 105.805, poly));
    TCU_REQUIRE(!common::isPointInsidePolygon(21.035, 105.805, poly));

    TCU_REQUIRE(common::compareSemVer("2.4.0", "2.3.9") > 0);
    TCU_REQUIRE(common::compareSemVer("2.0.0", "2.0.0") == 0);
    TCU_REQUIRE(common::compareSemVer("1.9.8", "2.0.0") < 0);

    // 4. NMEA-0183 GGA Sentence Parser (Production GNSS HAL)
    GnssFix nmea_fix{};
    const std::string gga = "$GNGGA,123519,2101.7106,N,10548.2890,E,1,11,0.9,14.2,M,0.0,M,,*61";
    // Compute valid NMEA XOR checksum dynamically
    std::string body = "GNGGA,123519,2101.7106,N,10548.2890,E,1,11,0.9,14.2,M,0.0,M,,";
    uint8_t csum = 0;
    for (char c : body) csum ^= static_cast<uint8_t>(c);
    char cs_hex[8];
    std::snprintf(cs_hex, sizeof(cs_hex), "*%02X", csum);
    TCU_REQUIRE(hal::Sa515mGnssHAL::parseNmeaGga("$" + body + cs_hex, nmea_fix));
    TCU_REQUIRE(nmea_fix.fix_valid);
    TCU_REQUIRE(nmea_fix.satellites == 11U);
    TCU_REQUIRE(std::fabs(nmea_fix.lat_deg - 21.02851) < 1e-4);

    // 5. OpenSSL 3.x Cryptographic Primitives (SHA-256, Ed25519, HMAC-SHA256, AES-256-GCM)
    hal::OpenSslCryptoHAL crypto;
    TCU_REQUIRE(crypto.sha256BufferHex("abc") ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    std::string key_dir = "/tmp/tcu_crypto_test_" + std::to_string(::getpid());
    std::filesystem::create_directories(key_dir);
    std::string priv_pem = key_dir + "/priv.pem";
    std::string pub_pem  = key_dir + "/pub.pem";
    TCU_REQUIRE(hal::OpenSslCryptoHAL::generateEd25519Keypair(priv_pem, pub_pem));

    std::string manifest_msg = "CAMP-2026|2.4.0|deadbeef";
    auto sig = hal::OpenSslCryptoHAL::signEd25519(manifest_msg, priv_pem);
    TCU_REQUIRE(sig.size() == 64U);
    TCU_REQUIRE(crypto.verifySignature(manifest_msg, sig, pub_pem));
    TCU_REQUIRE(!crypto.verifySignature(manifest_msg + "-tampered", sig, pub_pem));

    std::vector<uint8_t> aes_key(32, 0x42U);
    std::string secret_crumb = "{\"lat\":21.0285,\"lon\":105.8048,\"vin\":\"VN1TCU515M0000001\"}";
    auto ct = crypto.encryptAes256Gcm(secret_crumb, aes_key);
    TCU_REQUIRE(ct.size() > secret_crumb.size());
    TCU_REQUIRE(crypto.decryptAes256Gcm(ct, aes_key) == secret_crumb);
    // Tampering with GCM authentication tag must reject decryption
    ct.back() ^= 0xFFU;
    TCU_REQUIRE(crypto.decryptAes256Gcm(ct, aes_key).empty());

    std::filesystem::remove_all(key_dir);
    std::cout << "[PASS] test_common_protocols\n";
    return 0;
}

