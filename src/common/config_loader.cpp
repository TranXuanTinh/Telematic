#include "common/config_loader.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef ENABLE_YAML_CPP
#include <yaml-cpp/yaml.h>
#endif

namespace tcu::common {

TcuConfig ConfigLoader::load(const std::string& yaml_path) {
    TcuConfig cfg;
    if (yaml_path.empty() || !std::filesystem::exists(yaml_path)) {
        return cfg;
    }

#ifdef ENABLE_YAML_CPP
    try {
        YAML::Node root = YAML::LoadFile(yaml_path);
        if (auto n = root["system"]) {
            if (n["device_id"])   cfg.system.device_id   = n["device_id"].as<std::string>();
            if (n["vin"])         cfg.system.vin         = n["vin"].as<std::string>();
            if (n["hw_compat"])   cfg.system.hw_compat   = n["hw_compat"].as<std::string>();
            if (n["sw_version"])  cfg.system.sw_version  = n["sw_version"].as<std::string>();
            if (n["hal_mode"])    cfg.system.hal_mode    = n["hal_mode"].as<std::string>();
            if (n["log_level"])   cfg.system.log_level   = n["log_level"].as<std::string>();
            if (n["runtime_dir"]) cfg.system.runtime_dir = n["runtime_dir"].as<std::string>();
        }
        if (auto n = root["ipc"]) {
            if (n["broker_socket"])  cfg.ipc.broker_socket  = n["broker_socket"].as<std::string>();
            if (n["diag_socket"])    cfg.ipc.diag_socket    = n["diag_socket"].as<std::string>();
            if (n["rpc_timeout_ms"]) cfg.ipc.rpc_timeout_ms = n["rpc_timeout_ms"].as<int>();
        }
        if (auto n = root["mcu_link"]) {
            if (n["spi_device"])            cfg.mcu_link.spi_device            = n["spi_device"].as<std::string>();
            if (n["spi_speed_hz"])          cfg.mcu_link.spi_speed_hz          = n["spi_speed_hz"].as<uint32_t>();
            if (n["uart_device"])           cfg.mcu_link.uart_device           = n["uart_device"].as<std::string>();
            if (n["uart_baud"])             cfg.mcu_link.uart_baud             = n["uart_baud"].as<uint32_t>();
            if (n["mock_socket"])           cfg.mcu_link.mock_socket           = n["mock_socket"].as<std::string>();
            if (n["ack_timeout_ms"])        cfg.mcu_link.ack_timeout_ms        = n["ack_timeout_ms"].as<int>();
            if (n["max_retries"])           cfg.mcu_link.max_retries           = n["max_retries"].as<int>();
            if (n["heartbeat_interval_ms"]) cfg.mcu_link.heartbeat_interval_ms = n["heartbeat_interval_ms"].as<int>();
        }
        if (auto n = root["modem"]) {
            if (n["emergency_number"]) cfg.modem.emergency_number = n["emergency_number"].as<std::string>();
            if (n["test_psap_number"]) cfg.modem.test_psap_number = n["test_psap_number"].as<std::string>();
            if (n["apn"])              cfg.modem.apn              = n["apn"].as<std::string>();
        }
        if (auto n = root["gnss"]) {
            if (n["device"])            cfg.gnss.device            = n["device"].as<std::string>();
            if (n["normal_rate_hz"])    cfg.gnss.normal_rate_hz    = n["normal_rate_hz"].as<int>();
            if (n["emergency_rate_hz"]) cfg.gnss.emergency_rate_hz = n["emergency_rate_hz"].as<int>();
            if (n["default_lat"])       cfg.gnss.default_lat       = n["default_lat"].as<double>();
            if (n["default_lon"])       cfg.gnss.default_lon       = n["default_lon"].as<double>();
        }
        if (auto n = root["cloud"]) {
            if (n["mqtt_broker"])         cfg.cloud.mqtt_broker         = n["mqtt_broker"].as<std::string>();
            if (n["client_id"])           cfg.cloud.client_id           = n["client_id"].as<std::string>();
            if (n["ca_cert"])             cfg.cloud.ca_cert             = n["ca_cert"].as<std::string>();
            if (n["client_cert"])         cfg.cloud.client_cert         = n["client_cert"].as<std::string>();
            if (n["client_key"])          cfg.cloud.client_key          = n["client_key"].as<std::string>();
            if (n["command_hmac_secret"]) cfg.cloud.command_hmac_secret = n["command_hmac_secret"].as<std::string>();
            if (n["spool_dir"])           cfg.cloud.spool_dir           = n["spool_dir"].as<std::string>();
        }
        if (auto n = root["ecall"]) {
            if (n["sos_debounce_ms"])      cfg.ecall.sos_debounce_ms      = n["sos_debounce_ms"].as<int>();
            if (n["msd_tx_timeout_ms"])    cfg.ecall.msd_tx_timeout_ms    = n["msd_tx_timeout_ms"].as<int>();
            if (n["max_dial_retries"])     cfg.ecall.max_dial_retries     = n["max_dial_retries"].as<int>();
            if (n["callback_standby_sec"]) cfg.ecall.callback_standby_sec = n["callback_standby_sec"].as<int>();
            if (n["propulsion_mask"])      cfg.ecall.propulsion_mask      = static_cast<uint8_t>(n["propulsion_mask"].as<int>());
            if (n["vehicle_type"])         cfg.ecall.vehicle_type         = static_cast<uint8_t>(n["vehicle_type"].as<int>());
        }
        if (auto n = root["ota"]) {
            if (n["staging_dir"])                cfg.ota.staging_dir                = n["staging_dir"].as<std::string>();
            if (n["slot_state_file"])            cfg.ota.slot_state_file            = n["slot_state_file"].as<std::string>();
            if (n["public_key_pem"])             cfg.ota.public_key_pem             = n["public_key_pem"].as<std::string>();
            if (n["min_anti_rollback_version"])  cfg.ota.min_anti_rollback_version  = n["min_anti_rollback_version"].as<std::string>();
            if (n["min_battery_mv"])             cfg.ota.min_battery_mv             = n["min_battery_mv"].as<uint16_t>();
            if (n["min_bub_soc_pct"])            cfg.ota.min_bub_soc_pct            = static_cast<uint8_t>(n["min_bub_soc_pct"].as<int>());
            if (n["post_boot_bist_timeout_sec"]) cfg.ota.post_boot_bist_timeout_sec = n["post_boot_bist_timeout_sec"].as<int>();
        }
        if (auto n = root["svt"]) {
            if (n["geofence_radius_m"])             cfg.svt.geofence_radius_m             = n["geofence_radius_m"].as<double>();
            if (n["tilt_threshold_deg"])            cfg.svt.tilt_threshold_deg            = n["tilt_threshold_deg"].as<float>();
            if (n["kl30_cut_threshold_mv"])         cfg.svt.kl30_cut_threshold_mv         = n["kl30_cut_threshold_mv"].as<uint16_t>();
            if (n["moving_report_interval_ms"])     cfg.svt.moving_report_interval_ms     = n["moving_report_interval_ms"].as<int>();
            if (n["stationary_report_interval_ms"]) cfg.svt.stationary_report_interval_ms = n["stationary_report_interval_ms"].as<int>();
            if (n["zero_speed_hold_ms"])            cfg.svt.zero_speed_hold_ms            = n["zero_speed_hold_ms"].as<int>();
        }
        if (auto n = root["diagnostics"]) {
            if (n["bist_interval_sec"])    cfg.diagnostics.bist_interval_sec    = n["bist_interval_sec"].as<int>();
            if (n["min_bub_soh_pct"])      cfg.diagnostics.min_bub_soh_pct      = static_cast<uint8_t>(n["min_bub_soh_pct"].as<int>());
            if (n["gnss_ant_min_ma"])      cfg.diagnostics.gnss_ant_min_ma      = n["gnss_ant_min_ma"].as<float>();
            if (n["gnss_ant_max_ma"])      cfg.diagnostics.gnss_ant_max_ma      = n["gnss_ant_max_ma"].as<float>();
            if (n["watchdog_device"])      cfg.diagnostics.watchdog_device      = n["watchdog_device"].as<std::string>();
            if (n["watchdog_timeout_sec"]) cfg.diagnostics.watchdog_timeout_sec = n["watchdog_timeout_sec"].as<int>();
        }
        if (auto n = root["storage"]) {
            if (n["db_path"])            cfg.storage.db_path            = n["db_path"].as<std::string>();
            if (n["max_queue_records"])  cfg.storage.max_queue_records  = n["max_queue_records"].as<int>();
            if (n["encryption_key_hex"]) cfg.storage.encryption_key_hex = n["encryption_key_hex"].as<std::string>();
        }
    } catch (...) {
        // Fall back to safe defaults on malformed YAML
    }
#endif

    return cfg;
}

TcuConfig ConfigLoader::loadFromArgs(int argc, char* argv[], bool* force_mock_out) {
    std::string config_path = "config/tcu_config.yaml";
    std::string runtime_override;
    bool force_mock = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--mock") {
            force_mock = true;
        } else if (arg == "--runtime-dir" && i + 1 < argc) {
            runtime_override = argv[++i];
        }
    }

    TcuConfig cfg = load(config_path);
    if (force_mock) {
        cfg.system.hal_mode = "mock";
    }
    if (!runtime_override.empty()) {
        cfg.system.runtime_dir    = runtime_override;
        cfg.ipc.broker_socket     = runtime_override + "/ipc.sock";
        cfg.ipc.diag_socket       = runtime_override + "/diag.sock";
        cfg.mcu_link.mock_socket  = runtime_override + "/mcu_spi.sock";
        cfg.cloud.spool_dir       = runtime_override + "/cloud_spool";
        cfg.ota.staging_dir       = runtime_override + "/ota_staging";
        cfg.ota.slot_state_file   = runtime_override + "/rauc_slots.json";
        cfg.ota.public_key_pem    = runtime_override + "/ota_pub.pem";
        cfg.storage.db_path       = runtime_override + "/tcu_persistence.db";
    }
    if (force_mock_out) {
        *force_mock_out = (cfg.system.hal_mode == "mock");
    }
    std::error_code ec;
    std::filesystem::create_directories(cfg.system.runtime_dir, ec);
    return cfg;
}

} // namespace tcu::common

