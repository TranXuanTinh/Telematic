#include "app/ota_orchestrator.h"
#include "app/svt_orchestrator.h"
#include "common/config_loader.h"
#include "common/crc.h"
#include "common/json_mini.h"
#include "hal/sa515m/openssl_crypto_hal.h"
#include "ipc/ipc_client.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

void printUsage() {
    std::cout <<
        "tcu-ctl — Automotive TCU Diagnostic, Fault Injection & Control CLI\n"
        "Usage:\n"
        "  tcu-ctl [--config <yaml>] [--runtime-dir <dir>] <command> [args...]\n\n"
        "Commands:\n"
        "  status\n"
        "  diag run-bist\n"
        "  uds read-did <0xHEX> | read-dtcs | clear-dtcs | test-ecall | raw <HEX>\n"
        "  vehicle set [--speed <kmh>] [--ign on|off] [--doors locked|unlocked]\n"
        "              [--gear park|drive|reverse|neutral] [--kl30-mv <mV>] [--bub-soh <pct>]\n"
        "  inject crash [--airbag front|side|curtain] [--delta-v <kmh>] [--rollover]\n"
        "  inject sos [--hold-ms <ms>]\n"
        "  inject tamper [--kl30-cut] [--tilt-deg <deg>]\n"
        "  svt activate | engage-immobilizer [--signed-token <sig>] | disarm\n"
        "  ota generate-demo --dir <dir> [--version <ver>]\n"
        "  ota trigger --manifest <manifest.json>\n"
        "  ota commit-post-boot\n";
}

std::string readTextFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

uint16_t parseHexUint16(std::string s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
    }
    return static_cast<uint16_t>(std::stoul(s, nullptr, 16));
}

int sendUdsAndPrint(tcu::ipc::IpcClient& ipc, const std::string& req_hex) {
    tcu::common::JsonValue req = tcu::common::JsonValue::object();
    req["pdu_hex"] = req_hex;
    auto res = ipc.callRpc(
        tcu::ipc::DaemonId::DIAGD, tcu::ipc::RpcMethodId::PROCESS_UDS_PDU, req.dump(false), 2000);
    if (!res.ok()) {
        std::cerr << "[ERROR] UDS RPC failed (status=" << static_cast<int>(res.status) << ")\n";
        return 1;
    }
    auto rj = tcu::common::JsonValue::parse(res.payload);
    if (!rj) {
        std::cout << res.payload << "\n";
        return 0;
    }
    std::string pdu_hex = (*rj)["pdu_hex"].asString();
    auto bytes = tcu::common::hexToBytes(pdu_hex);
    std::cout << "UDS TX : " << req_hex << "\n";
    std::cout << "UDS RX : " << pdu_hex;
    if (!bytes.empty() && bytes[0] == 0x7FU && bytes.size() >= 3) {
        std::cout << "  [NEGATIVE RESPONSE NRC=0x" << std::hex << std::uppercase
                  << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[2])
                  << std::dec << "]\n";
        return 2;
    }
    if (bytes.size() > 3 && bytes[0] == 0x62U) {
        std::string ascii_preview;
        bool printable = true;
        for (size_t i = 3; i < bytes.size(); ++i) {
            if (bytes[i] >= 32U && bytes[i] <= 126U) {
                ascii_preview.push_back(static_cast<char>(bytes[i]));
            } else {
                printable = false;
                break;
            }
        }
        if (printable && !ascii_preview.empty()) {
            std::cout << "  (ASCII: \"" << ascii_preview << "\")";
        }
    }
    std::cout << "\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    auto cfg = tcu::common::ConfigLoader::loadFromArgs(argc, argv, nullptr);

    // Collect positional / subcommand tokens (skipping global --config / --runtime-dir / --mock)
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--config" || a == "-c" || a == "--runtime-dir") && (i + 1) < argc) {
            ++i;
            continue;
        }
        if (a == "--mock") {
            continue;
        }
        if (a == "--broker" && (i + 1) < argc) {
            cfg.ipc.broker_socket = argv[++i];
            continue;
        }
        if (a == "--help" || a == "-h") {
            printUsage();
            return 0;
        }
        args.push_back(a);
    }

    if (args.empty()) {
        printUsage();
        return 1;
    }

    const std::string group = args[0];

    // Offline helper: ota generate-demo --dir <dir> [--version 2.4.0]
    if (group == "ota" && args.size() >= 2 && args[1] == "generate-demo") {
        std::string out_dir = "/tmp/demo_ota_2.4.0";
        std::string ver     = "2.4.0";
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--dir" && (i + 1) < args.size()) out_dir = args[++i];
            else if (args[i] == "--version" && (i + 1) < args.size()) ver = args[++i];
        }
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        auto pub_parent = std::filesystem::path(cfg.ota.public_key_pem).parent_path();
        if (!pub_parent.empty()) std::filesystem::create_directories(pub_parent, ec);

        const std::string priv_pem   = out_dir + "/ota_signing_priv.pem";
        const std::string linux_path = out_dir + "/tcu-rootfs-" + ver + ".raucb";
        const std::string mcu_path   = out_dir + "/mcu-s32k144-" + ver + ".bin";

        if (!tcu::hal::OpenSslCryptoHAL::generateEd25519Keypair(priv_pem, cfg.ota.public_key_pem)) {
            std::cerr << "[ERROR] Failed to generate Ed25519 keypair\n";
            return 1;
        }

        std::string dummy_rootfs(4096, 'R');
        dummy_rootfs += "RAUC-BUNDLE-" + ver;
        {
            std::ofstream ofs(linux_path, std::ios::binary | std::ios::trunc);
            ofs.write(dummy_rootfs.data(), static_cast<std::streamsize>(dummy_rootfs.size()));
        }

        std::vector<uint8_t> dummy_mcu(1536, 0xA5U);
        {
            std::ofstream ofs(mcu_path, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(dummy_mcu.data()),
                      static_cast<std::streamsize>(dummy_mcu.size()));
        }

        tcu::hal::OpenSslCryptoHAL crypto;
        tcu::OtaManifest m;
        m.campaign_id               = "CAMP-" + ver;
        m.bundle_version            = ver;
        m.min_anti_rollback_version = cfg.ota.min_anti_rollback_version;
        m.hw_compatibility          = {cfg.system.hw_compat, "qemuarm64-tcu"};
        m.linux_bundle_url          = "file://" + linux_path;
        m.linux_bundle_filename     = "tcu-rootfs-" + ver + ".raucb";
        m.linux_bundle_size         = dummy_rootfs.size();
        m.linux_bundle_sha256       = crypto.sha256FileHex(linux_path);
        m.mcu_firmware_present      = true;
        m.mcu_firmware_url          = "file://" + mcu_path;
        m.mcu_firmware_filename     = "mcu-s32k144-" + ver + ".bin";
        m.mcu_firmware_version      = "1.8.0";
        m.mcu_firmware_size         = static_cast<uint32_t>(dummy_mcu.size());
        m.mcu_firmware_crc32        = tcu::common::computeCrc32(dummy_mcu);
        m.mcu_firmware_sha256       = crypto.sha256FileHex(mcu_path);

        auto sig = tcu::hal::OpenSslCryptoHAL::signEd25519(
            tcu::app::OtaOrchestrator::canonicalSigningString(m), priv_pem);
        m.manifest_signature_hex = tcu::common::bytesToHex(sig);

        const std::string manifest_path = out_dir + "/manifest.json";
        {
            std::ofstream ofs(manifest_path, std::ios::binary | std::ios::trunc);
            ofs << tcu::app::OtaOrchestrator::serializeManifestJson(m) << "\n";
        }
        std::cout << "Generated signed Dual-Domain OTA bundle at: " << manifest_path << "\n";
        return 0;
    }

    // Connect to Intra-SoC UDS Broker hosted by tcu-mcu-ipcd
    tcu::ipc::IpcClient ipc(tcu::ipc::DaemonId::TCU_CTL, cfg.ipc.broker_socket);
    if (!ipc.connect(1500)) {
        std::cerr << "[ERROR] Cannot connect to TCU UDS Broker at " << cfg.ipc.broker_socket << "\n";
        return 1;
    }

    if (group == "status") {
        const std::pair<const char*, tcu::ipc::DaemonId> targets[] = {
            {"tcu-mcu-ipcd",  tcu::ipc::DaemonId::MCU_IPCD},
            {"tcu-platformd", tcu::ipc::DaemonId::PLATFORMD},
            {"tcu-ecalld",    tcu::ipc::DaemonId::ECALLD},
            {"tcu-otad",      tcu::ipc::DaemonId::OTAD},
            {"tcu-svtd",      tcu::ipc::DaemonId::SVTD},
            {"tcu-diagd",     tcu::ipc::DaemonId::DIAGD},
        };
        for (const auto& [name, did] : targets) {
            auto r = ipc.callRpc(did, tcu::ipc::RpcMethodId::GET_DAEMON_STATUS, "{}", 500);
            if (r.ok()) {
                std::cout << "[ONLINE ] " << std::left << std::setw(14) << name << " : "
                          << r.payload << "\n";
            } else {
                std::cout << "[OFFLINE] " << std::left << std::setw(14) << name << "\n";
            }
        }
        return 0;
    }

    if (group == "diag" && args.size() >= 2 && args[1] == "run-bist") {
        auto r = ipc.callRpc(tcu::ipc::DaemonId::DIAGD, tcu::ipc::RpcMethodId::RUN_BIST_NOW, "{}", 2500);
        if (!r.ok()) {
            std::cerr << "[ERROR] RUN_BIST_NOW RPC failed\n";
            return 1;
        }
        if (auto j = tcu::common::JsonValue::parse(r.payload)) {
            std::cout << j->dump(true) << "\n";
        } else {
            std::cout << r.payload << "\n";
        }
        return 0;
    }

    if (group == "uds" && args.size() >= 2) {
        const std::string sub = args[1];
        if (sub == "read-did" && args.size() >= 3) {
            uint16_t did = parseHexUint16(args[2]);
            uint8_t req_bytes[3] = {
                0x22U,
                static_cast<uint8_t>((did >> 8U) & 0xFFU),
                static_cast<uint8_t>(did & 0xFFU)};
            return sendUdsAndPrint(ipc, tcu::common::bytesToHex(req_bytes, 3));
        }
        if (sub == "read-dtcs") {
            return sendUdsAndPrint(ipc, "190209");
        }
        if (sub == "clear-dtcs") {
            sendUdsAndPrint(ipc, "1003");
            return sendUdsAndPrint(ipc, "14FFFFFF");
        }
        if (sub == "test-ecall") {
            sendUdsAndPrint(ipc, "1003");
            return sendUdsAndPrint(ipc, "31010201");
        }
        if (sub == "raw" && args.size() >= 3) {
            return sendUdsAndPrint(ipc, args[2]);
        }
    }

    if (group == "vehicle" && args.size() >= 2 && args[1] == "set") {
        tcu::common::JsonValue j = tcu::common::JsonValue::object();
        j["cmd"] = "set_vehicle";
        for (size_t i = 2; i < args.size(); ++i) {
            const std::string& k = args[i];
            if (k == "--speed" && (i + 1) < args.size()) {
                j["speed_kmh"] = std::stod(args[++i]);
            } else if (k == "--ign" && (i + 1) < args.size()) {
                std::string v = args[++i];
                j["ign"] = (v == "on" || v == "1" || v == "true");
            } else if (k == "--doors" && (i + 1) < args.size()) {
                std::string v = args[++i];
                j["doors_locked"] = (v == "locked" || v == "1" || v == "true");
            } else if (k == "--gear" && (i + 1) < args.size()) {
                std::string g = args[++i];
                int gv = 0;
                if (g == "reverse") gv = 1;
                else if (g == "neutral") gv = 2;
                else if (g == "drive") gv = 3;
                j["gear"] = gv;
            } else if (k == "--kl30-mv" && (i + 1) < args.size()) {
                j["kl30_mv"] = static_cast<uint64_t>(std::stoul(args[++i]));
            } else if (k == "--bub-soh" && (i + 1) < args.size()) {
                j["bub_soh"] = static_cast<uint64_t>(std::stoul(args[++i]));
            } else if (k == "--auth-driver" && (i + 1) < args.size()) {
                std::string v = args[++i];
                j["auth_driver"] = (v == "on" || v == "1" || v == "true");
            }
        }
        auto r = ipc.callRpc(
            tcu::ipc::DaemonId::MCU_IPCD, tcu::ipc::RpcMethodId::INJECT_TEST_EVENT, j.dump(false), 1000);
        std::cout << (r.ok() ? "Vehicle state updated: " : "[ERROR] ") << r.payload << "\n";
        return r.ok() ? 0 : 1;
    }

    if (group == "inject" && args.size() >= 2) {
        const std::string sub = args[1];
        if (sub == "crash") {
            int delta_v   = -48;
            bool rollover = false;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--delta-v" && (i + 1) < args.size()) {
                    int v = std::stoi(args[++i]);
                    delta_v = (v > 0) ? -v : v;
                } else if (args[i] == "--airbag" && (i + 1) < args.size()) {
                    ++i; // front/side/curtain accepted
                } else if (args[i] == "--rollover") {
                    rollover = true;
                }
            }
            tcu::common::JsonValue j = tcu::common::JsonValue::object();
            j["cmd"]      = "crash";
            j["auto"]     = true;
            j["delta_v"]  = delta_v;
            j["rollover"] = rollover;
            auto r = ipc.callRpc(
                tcu::ipc::DaemonId::MCU_IPCD, tcu::ipc::RpcMethodId::INJECT_TEST_EVENT, j.dump(false), 1000);
            std::cout << (r.ok() ? "Crash IRQ injected: " : "[ERROR] ") << r.payload << "\n";
            return r.ok() ? 0 : 1;
        }
        if (sub == "sos") {
            int hold_ms = 2200;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--hold-ms" && (i + 1) < args.size()) {
                    hold_ms = std::stoi(args[++i]);
                }
            }
            ipc.callRpc(tcu::ipc::DaemonId::ECALLD,
                        tcu::ipc::RpcMethodId::INJECT_TEST_EVENT,
                        "{\"cmd\":\"sos_press\"}",
                        500);
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
            auto r = ipc.callRpc(tcu::ipc::DaemonId::ECALLD,
                                 tcu::ipc::RpcMethodId::INJECT_TEST_EVENT,
                                 "{\"cmd\":\"sos_release\"}",
                                 500);
            std::cout << "Manual SOS hold (" << hold_ms << " ms) completed: " << r.payload << "\n";
            return 0;
        }
        if (sub == "tamper") {
            uint8_t flags  = 0U;
            double tilt_deg = 0.0;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--kl30-cut") {
                    flags |= tcu::TAMPER_KL30_BATTERY_CUT;
                } else if (args[i] == "--tilt-deg" && (i + 1) < args.size()) {
                    flags |= tcu::TAMPER_IMU_TOW_TILT;
                    tilt_deg = std::stod(args[++i]);
                }
            }
            if (flags == 0U) flags = tcu::TAMPER_KL30_BATTERY_CUT;
            tcu::common::JsonValue j = tcu::common::JsonValue::object();
            j["cmd"]       = "tamper";
            j["flags"]     = static_cast<int>(flags);
            j["pitch_deg"] = tilt_deg;
            auto r = ipc.callRpc(
                tcu::ipc::DaemonId::MCU_IPCD, tcu::ipc::RpcMethodId::INJECT_TEST_EVENT, j.dump(false), 1000);
            std::cout << (r.ok() ? "Tamper event injected: " : "[ERROR] ") << r.payload << "\n";
            return r.ok() ? 0 : 1;
        }
    }

    if (group == "svt" && args.size() >= 2) {
        std::string sub = args[1];
        std::string action;
        if (sub == "activate") action = "SVT_ACTIVATE";
        else if (sub == "engage-immobilizer") action = "SVT_IMMOBILIZE";
        else if (sub == "disarm") action = "SVT_DISARM_RECOVERED";
        else {
            printUsage();
            return 1;
        }

        tcu::hal::OpenSslCryptoHAL crypto;
        uint64_t nonce = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        std::string cmd_json = tcu::app::SvtOrchestrator::buildSignedCloudCommand(
            crypto, cfg.cloud.command_hmac_secret, action, nonce);

        auto r = ipc.callRpc(
            tcu::ipc::DaemonId::SVTD, tcu::ipc::RpcMethodId::INJECT_TEST_EVENT, cmd_json, 1000);
        std::cout << (r.ok() ? "SVT command accepted (" + action + "): " : "[REJECTED] ")
                  << r.payload << "\n";
        return r.ok() ? 0 : 1;
    }

    if (group == "ota" && args.size() >= 2) {
        const std::string sub = args[1];
        if (sub == "trigger") {
            std::string manifest_path;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--manifest" && (i + 1) < args.size()) {
                    manifest_path = args[++i];
                }
            }
            std::string json_text = readTextFile(manifest_path);
            if (json_text.empty()) {
                std::cerr << "[ERROR] Cannot read OTA manifest file: " << manifest_path << "\n";
                return 1;
            }
            auto r = ipc.callRpc(
                tcu::ipc::DaemonId::OTAD, tcu::ipc::RpcMethodId::TRIGGER_OTA_CAMPAIGN, json_text, 2000);
            std::cout << (r.ok() ? "OTA campaign submitted: " : "[ERROR] ") << r.payload << "\n";
            return r.ok() ? 0 : 1;
        }
        if (sub == "commit-post-boot") {
            auto r = ipc.callRpc(
                tcu::ipc::DaemonId::OTAD, tcu::ipc::RpcMethodId::COMMIT_POST_BOOT_OTA, "{}", 2500);
            std::cout << "Post-boot OTA verification result: " << r.payload << "\n";
            return r.ok() ? 0 : 1;
        }
    }

    printUsage();
    return 1;
}

