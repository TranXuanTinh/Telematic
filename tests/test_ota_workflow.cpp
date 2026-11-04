#include "common/crc.h"
#include "common/json_mini.h"
#include "test_cluster_harness.h"

#include <fstream>
#include <iostream>
#include <vector>

namespace {

tcu::OtaManifest createSignedBundle(const tcu::test::TcuTestCluster& cluster,
                                    const std::string& priv_pem,
                                    const std::string& version,
                                    bool with_mcu = true) {
    const std::string art_dir = cluster.runtime_dir + "/repo_" + version;
    std::filesystem::create_directories(art_dir);

    const std::string lin_file = "rootfs-" + version + ".raucb";
    const std::string lin_path = art_dir + "/" + lin_file;
    std::string lin_data(2048, 'L');
    lin_data += version;
    {
        std::ofstream ofs(lin_path, std::ios::binary | std::ios::trunc);
        ofs.write(lin_data.data(), static_cast<std::streamsize>(lin_data.size()));
    }

    const std::string mcu_file = "mcu-" + version + ".bin";
    const std::string mcu_path = art_dir + "/" + mcu_file;
    std::vector<uint8_t> mcu_data(1200, 0x5AU);
    {
        std::ofstream ofs(mcu_path, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(mcu_data.data()),
                  static_cast<std::streamsize>(mcu_data.size()));
    }

    tcu::OtaManifest m;
    m.campaign_id               = "CAMP-" + version;
    m.bundle_version            = version;
    m.min_anti_rollback_version = "2.0.0";
    m.hw_compatibility          = {cluster.cfg.system.hw_compat};
    m.linux_bundle_url          = "file://" + lin_path;
    m.linux_bundle_filename     = lin_file;
    m.linux_bundle_size         = lin_data.size();
    m.linux_bundle_sha256       = cluster.crypto_hal.sha256FileHex(lin_path);

    m.mcu_firmware_present      = with_mcu;
    m.mcu_firmware_url          = "file://" + mcu_path;
    m.mcu_firmware_filename     = mcu_file;
    m.mcu_firmware_version      = "1.9.0";
    m.mcu_firmware_size         = static_cast<uint32_t>(mcu_data.size());
    m.mcu_firmware_crc32        = tcu::common::computeCrc32(mcu_data);
    m.mcu_firmware_sha256       = cluster.crypto_hal.sha256FileHex(mcu_path);

    auto sig = tcu::hal::OpenSslCryptoHAL::signEd25519(
        tcu::app::OtaOrchestrator::canonicalSigningString(m), priv_pem);
    m.manifest_signature_hex = tcu::common::bytesToHex(sig);
    return m;
}

} // namespace

int main() {
    using namespace tcu;

    test::TcuTestCluster cluster("ota");
    const std::string priv_pem = cluster.runtime_dir + "/ota_priv.pem";
    TCU_REQUIRE(hal::OpenSslCryptoHAL::generateEd25519Keypair(
        priv_pem, cluster.cfg.ota.public_key_pem));
    TCU_REQUIRE(cluster.startAll());

    // 1. Forged/Tampered Signature Rejection
    auto forged = createSignedBundle(cluster, priv_pem, "2.4.0");
    forged.manifest_signature_hex[0] = (forged.manifest_signature_hex[0] == 'a') ? 'b' : 'a';
    cluster.otad->submitCampaign(forged);
    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.otad->currentState() == OtaState::ABORTED_REPORT;
    }, 2000));
    TCU_REQUIRE(cluster.otad->lastFailureReason() == "signature_verification_failed");

    // 2. Anti-Rollback Downgrade Rejection (v2.1.0 <= installed v2.3.0)
    auto downgrade = createSignedBundle(cluster, priv_pem, "2.1.0");
    cluster.otad->submitCampaign(downgrade);
    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.otad->currentState() == OtaState::ABORTED_REPORT &&
               cluster.otad->lastFailureReason() == "anti_rollback_version_violation";
    }, 2000));

    // 3. Vehicle Safety Gate Enforcement: Driving at 75 km/h blocks flashing in WAITING_SAFETY_GATE
    VehicleState driving{};
    driving.speed_kmh     = 75.0;
    driving.ignition_kl15 = true;
    driving.gear          = GearPosition::DRIVE;
    driving.kl30_mv       = 13800;
    driving.bub_soc       = 95;
    cluster.mcu_sim->setVehicleState(driving);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    auto valid_240 = createSignedBundle(cluster, priv_pem, "2.4.0", true);
    cluster.otad->submitCampaign(valid_240);
    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.otad->currentState() == OtaState::WAITING_SAFETY_GATE;
    }, 2500));
    TCU_REQUIRE(cluster.mcu_sim->activeFlashBank() == 0U); // Not flashed while moving!

    // 4. Driver Parks & Turns Ignition OFF -> Dual-Domain Flash (MCU Bank B + Linux Slot B) + Commit
    VehicleState parked{};
    parked.speed_kmh     = 0.0;
    parked.ignition_kl15 = false;
    parked.gear          = GearPosition::PARK;
    parked.kl30_mv       = 12600;
    parked.bub_soc       = 95;
    cluster.mcu_sim->setVehicleState(parked);

    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.otad->currentState() == OtaState::COMMITTED_GOOD;
    }, 4000));

    TCU_REQUIRE(cluster.mcu_sim->activeFlashBank() == 1U);
    auto slots_after_240 = cluster.ota_boot_hal->getSlotStatus();
    TCU_REQUIRE(slots_after_240.active_slot == "rootfs.1");
    TCU_REQUIRE(slots_after_240.installed_version == "2.4.0");
    TCU_REQUIRE(slots_after_240.active_marked_good == true);

    // 5. Defective Update v2.5.0 -> Post-Boot BIST Fails -> Automatic Rollback to Slot B (v2.4.0)!
    cluster.diagd->setSimulatedPostBootFailure(true);
    auto buggy_250 = createSignedBundle(cluster, priv_pem, "2.5.0", false);
    cluster.otad->submitCampaign(buggy_250);
    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.otad->currentState() == OtaState::AUTOMATIC_ROLLBACK;
    }, 4000));

    auto rolled_back = cluster.ota_boot_hal->getSlotStatus();
    TCU_REQUIRE(rolled_back.active_slot == "rootfs.1");
    TCU_REQUIRE(rolled_back.installed_version == "2.4.0");
    TCU_REQUIRE(rolled_back.rollback_count >= 1U);

    std::cout << "[PASS] test_ota_workflow\n";
    return 0;
}
