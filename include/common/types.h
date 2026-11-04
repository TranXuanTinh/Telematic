#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace tcu {

// ============================================================================
// Power & Companion MCU Domain Enumerations
// ============================================================================

enum class PowerState : uint8_t {
    SLEEP           = 0,
    STARTUP         = 1,
    ACTIVE_RUN      = 2,
    POST_RUN        = 3,
    OTA_MAINTENANCE = 4,
    PRE_SLEEP       = 5
};

enum class WakeupReason : uint8_t {
    NONE                = 0,
    KL15_IGN_ON         = 1,
    CAN_SELECTIVE_WAKE  = 2,
    SOS_BUTTON          = 3,
    CRASH_AIRBAG        = 4,
    TAMPER_KL30_CUT     = 5,
    IMU_TOW_TILT        = 6,
    MODEM_RING          = 7,
    RTC_TIMER           = 8
};

enum class GearPosition : uint8_t {
    PARK    = 0,
    REVERSE = 1,
    NEUTRAL = 2,
    DRIVE   = 3
};

enum class EcallLedMode : uint8_t {
    OFF         = 0,
    GREEN_READY = 1,
    BLINK_CALL  = 2,
    RED_FAULT   = 3
};

// ============================================================================
// Vehicle, Crash & Security Tamper Structures
// ============================================================================

struct VehicleState {
    double       speed_kmh{0.0};
    bool         ignition_kl15{false};
    GearPosition gear{GearPosition::PARK};
    bool         doors_locked{true};
    bool         authorized_driver{false};
    uint16_t     kl30_mv{12600};          // Main 12V battery voltage in mV
    uint16_t     bub_mv{4100};            // Backup Battery voltage in mV
    uint8_t      bub_soc{95};             // Backup Battery State-of-Charge (%)
    uint8_t      bub_soh{98};             // Backup Battery State-of-Health (%)
    float        cpu_temp_c{42.0f};
    bool         using_backup_battery{false};
    bool         starter_inhibit_active{false};
    uint64_t     timestamp_utc{0};
};

struct CrashEvent {
    bool     automatic_activation{true};  // true = Airbag/IMU, false = Manual SOS
    bool     test_call{false};            // true = UDS 0x31 / CLI test PSAP call
    uint8_t  airbag_mask{0x01};           // Bit0=Front, Bit1=Side, Bit2=Curtain
    int16_t  delta_v_x_kmh{-48};
    int16_t  delta_v_y_kmh{0};
    bool     rollover{false};
    uint8_t  passengers{1};
    uint64_t timestamp_utc{0};
};

enum TamperFlag : uint8_t {
    TAMPER_NONE              = 0x00,
    TAMPER_KL30_BATTERY_CUT  = 0x01,
    TAMPER_IMU_TOW_TILT      = 0x02,
    TAMPER_GNSS_JAMMING      = 0x04,
    TAMPER_GEOFENCE_BREACH   = 0x08,
    TAMPER_REMOTE_DECLARATION= 0x10
};

struct TamperEvent {
    uint8_t  tamper_flags{TAMPER_NONE};
    float    tilt_pitch_deg{0.0f};
    float    tilt_roll_deg{0.0f};
    double   breach_distance_m{0.0};
    uint64_t timestamp_utc{0};
};

// ============================================================================
// GNSS, Cellular Modem & Audio Structures
// ============================================================================

enum class AntennaHealth : uint8_t {
    OK                = 0,
    OPEN_CIRCUIT      = 1,
    SHORT_CIRCUIT     = 2,
    JAMMING_SUSPECTED = 3
};

struct GnssFix {
    double        lat_deg{0.0};
    double        lon_deg{0.0};
    double        alt_m{12.0};
    float         heading_deg{0.0f};      // 0..359.9 degrees
    float         speed_mps{0.0f};
    float         hdop{0.9f};
    uint8_t       satellites{11};
    bool          fix_valid{true};
    AntennaHealth antenna_health{AntennaHealth::OK};
    float         antenna_current_ma{18.5f};
    uint64_t      timestamp_utc{0};
};

struct GnssTrail {
    GnssFix current;
    GnssFix n1;
    GnssFix n2;
    bool    has_n1{false};
    bool    has_n2{false};
};

struct CellTowerInfo {
    uint16_t mcc{452};
    uint16_t mnc{4};
    uint32_t tac{1020};
    uint64_t cell_id{8841201};
    int16_t  rsrp_dbm{-82};
};

struct ModemStatus {
    bool                       sim_ready{true};
    bool                       registered{true};
    bool                       roaming{false};
    std::string                rat{"LTE"};
    int16_t                    rssi_dbm{-75};
    std::string                imei{"359876100051501"};
    CellTowerInfo              serving_cell{};
    std::vector<CellTowerInfo> neighbor_cells{};
    bool                       voice_call_active{false};
    std::string                connected_peer{};
};

enum class AudioRoute : uint8_t {
    MUTED                = 0,
    MODEM_VOICE_TO_CABIN = 1,
    DIAGNOSTIC_LOOPBACK  = 2
};

struct AudioLoopbackResult {
    bool  passed{true};
    float snr_db{38.5f};
    float mic_bias_mv{2200.0f};
    float speaker_impedance_ohm{8.1f};
};

// ============================================================================
// Feature State Machines (eCall, OTA, SVT, Diagnostics)
// ============================================================================

enum class EcallState : uint8_t {
    IDLE_ARMED       = 0,
    SOS_DEBOUNCE     = 1,
    PREEMPT_AND_LOCK = 2,
    BUILD_MSD        = 3,
    DIALING_PSAP     = 4,
    TRANSMITTING_MSD = 5,
    VOICE_ACTIVE     = 6,
    RETRY_BACKOFF    = 7,
    CALLBACK_STANDBY = 8
};

enum class OtaState : uint8_t {
    IDLE                = 0,
    DOWNLOADING         = 1,
    DOWNLOAD_PAUSED     = 2,
    VERIFYING           = 3,
    WAITING_SAFETY_GATE = 4,
    FLASHING_MCU        = 5,
    INSTALLING_LINUX_AB = 6,
    REBOOTING_SLOT      = 7,
    POST_BOOT_VERIFY    = 8,
    COMMITTED_GOOD      = 9,
    AUTOMATIC_ROLLBACK  = 10,
    ABORTED_REPORT      = 11
};

struct OtaManifest {
    std::string              campaign_id;
    std::string              bundle_version;
    std::string              min_anti_rollback_version;
    std::vector<std::string> hw_compatibility;

    std::string              linux_bundle_url;
    std::string              linux_bundle_filename;
    uint64_t                 linux_bundle_size{0};
    std::string              linux_bundle_sha256;

    bool                     mcu_firmware_present{false};
    std::string              mcu_firmware_url;
    std::string              mcu_firmware_filename;
    std::string              mcu_firmware_version;
    uint32_t                 mcu_firmware_size{0};
    uint32_t                 mcu_firmware_crc32{0};
    std::string              mcu_firmware_sha256;

    std::string              manifest_signature_hex; // Ed25519/RSA detached sig over canonical fields
};

enum class SvtState : uint8_t {
    DISARMED           = 0,
    ARMED              = 1,
    PRE_ALARM          = 2,
    STOLEN_ACTIVE      = 3,
    IMMOBILIZE_PENDING = 4,
    IMMOBILIZED        = 5
};

struct BistSummary {
    bool         mcu_ipc_ok{true};
    bool         modem_ok{true};
    bool         gnss_antenna_ok{true};
    bool         audio_loopback_ok{true};
    bool         storage_ok{true};
    bool         bub_ok{true};
    bool         ota_slot_healthy{true};
    uint16_t     active_dtc_count{0};
    EcallLedMode ecall_led{EcallLedMode::GREEN_READY};
    uint64_t     timestamp_utc{0};

    bool allCriticalOk() const noexcept {
        return mcu_ipc_ok && modem_ok && gnss_antenna_ok &&
               audio_loopback_ok && storage_ok && bub_ok && ota_slot_healthy;
    }
};

struct DtcRecord {
    std::string dtc_code;          // e.g. "B1A01-13"
    uint8_t     status_mask{0x09}; // Bit0=testFailed, Bit3=confirmedDTC
    uint64_t    first_seen_utc{0};
    uint64_t    last_seen_utc{0};
    uint32_t    occurrence_count{1};
    std::string freeze_frame_json;
};

// ============================================================================
// Master Configuration Object
// ============================================================================

struct TcuConfig {
    struct {
        std::string device_id{"tcu-sa515m-001"};
        std::string vin{"VN1TCU515M0000001"};
        std::string hw_compat{"sa515m-s32k144-revB"};
        std::string sw_version{"2.3.0"};
        std::string hal_mode{"mock"};
        std::string log_level{"info"};
        std::string runtime_dir{"/tmp/tcu"};
    } system;

    struct {
        std::string broker_socket{"/tmp/tcu/ipc.sock"};
        std::string diag_socket{"/tmp/tcu/diag.sock"};
        int         rpc_timeout_ms{1500};
    } ipc;

    struct {
        std::string spi_device{"/dev/spidev0.0"};
        uint32_t    spi_speed_hz{10000000};
        std::string uart_device{"/dev/ttyHS0"};
        uint32_t    uart_baud{921600};
        std::string mock_socket{"/tmp/tcu/mcu_spi.sock"};
        int         ack_timeout_ms{50};
        int         max_retries{3};
        int         heartbeat_interval_ms{2000};
    } mcu_link;

    struct {
        std::string emergency_number{"112"};
        std::string test_psap_number{"+8419001120"};
        std::string apn{"telematics.oem.net"};
    } modem;

    struct {
        std::string device{"/dev/ttyGNSS0"};
        int         normal_rate_hz{1};
        int         emergency_rate_hz{10};
        double      default_lat{21.028511};
        double      default_lon{105.804817};
    } gnss;

    struct {
        std::string mqtt_broker{"ssl://mqtt.telematics.oem.local:8883"};
        std::string client_id{"tcu-sa515m-001"};
        std::string ca_cert{"/factory/ca.crt"};
        std::string client_cert{"/factory/device.crt"};
        std::string client_key{"/factory/device.key"};
        std::string command_hmac_secret{"oem-soc-signing-key-2026-v1"};
        std::string spool_dir{"/tmp/tcu/cloud_spool"};
    } cloud;

    struct {
        int     sos_debounce_ms{2000};
        int     msd_tx_timeout_ms{20000};
        int     max_dial_retries{10};
        int     callback_standby_sec{3600};
        uint8_t propulsion_mask{0x04};
        uint8_t vehicle_type{1};
    } ecall;

    struct {
        std::string staging_dir{"/tmp/tcu/ota_staging"};
        std::string slot_state_file{"/tmp/tcu/rauc_slots.json"};
        std::string public_key_pem{"/tmp/tcu/ota_pub.pem"};
        std::string min_anti_rollback_version{"2.0.0"};
        uint16_t    min_battery_mv{12200};
        uint8_t     min_bub_soc_pct{50};
        int         post_boot_bist_timeout_sec{90};
    } ota;

    struct {
        double   geofence_radius_m{100.0};
        float    tilt_threshold_deg{3.0f};
        uint16_t kl30_cut_threshold_mv{6000};
        int      moving_report_interval_ms{5000};
        int      stationary_report_interval_ms{60000};
        int      zero_speed_hold_ms{3000};
    } svt;

    struct {
        int         bist_interval_sec{30};
        uint8_t     min_bub_soh_pct{70};
        float       gnss_ant_min_ma{2.0f};
        float       gnss_ant_max_ma{45.0f};
        std::string watchdog_device{"/dev/watchdog"};
        int         watchdog_timeout_sec{15};
    } diagnostics;

    struct {
        std::string db_path{"/tmp/tcu/tcu_persistence.db"};
        int         max_queue_records{50000};
        std::string encryption_key_hex{
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    } storage;
};

inline uint64_t currentEpochSeconds() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

inline uint64_t steadyNowMs() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace tcu

