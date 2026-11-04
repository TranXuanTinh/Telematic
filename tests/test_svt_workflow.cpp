#include "test_cluster_harness.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace tcu;

    test::TcuTestCluster cluster("svt");
    TCU_REQUIRE(cluster.startAll());

    // 1. Auto-Arm on Vehicle Parked & Doors Locked
    VehicleState parked{};
    parked.speed_kmh         = 0.0;
    parked.ignition_kl15     = false;
    parked.doors_locked      = true;
    parked.authorized_driver = false;
    parked.kl30_mv           = 12600;
    cluster.mcu_sim->setVehicleState(parked);

    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.svtd->currentState() == SvtState::ARMED;
    }, 2000));

    // 2. Cellular Jamming / Tunnel Offline + Thief Cuts 12V Battery & Tows 350m Outside Geofence
    cluster.cloud_hal->setOnline(false);
    cluster.cloud_hal->clearPublishedMessages();

    cluster.mcu_sim->injectTamper(TAMPER_KL30_BATTERY_CUT);
    cluster.gnss_hal.setSimulatedPosition(21.032000, 105.804817, 15.0f, 12.0f); // ~388m North

    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.svtd->currentState() == SvtState::STOLEN_ACTIVE &&
               cluster.platformd->offlineQueueDepth() >= 1U;
    }, 2500));

    // Verify Stealth Mode (cabin LED forced OFF, GNSS boosted to 10Hz)
    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.mcu_sim->getEcallLedMode() == EcallLedMode::OFF &&
               cluster.gnss_hal.currentRateHz() == 10;
    }, 1500));

    // 3. Reconnect Cellular Network -> Encrypted SQLite Breadcrumbs Flush Automatically to Cloud!
    cluster.cloud_hal->setOnline(true);
    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.platformd->offlineQueueDepth() == 0U &&
               !cluster.cloud_hal->publishedMessages().empty();
    }, 2500));

    // 4. Forged HMAC Command Rejected; Valid SOC Immobilizer Command Deferred While Speed == 65 km/h!
    TCU_REQUIRE(!cluster.svtd->handleAuthenticatedSocCommand(
        "{\"action\":\"SVT_IMMOBILIZE\",\"nonce\":100,\"hmac\":\"deadbeef\"}"));

    VehicleState thief_driving = parked;
    thief_driving.speed_kmh     = 65.0;
    thief_driving.ignition_kl15 = true;
    thief_driving.gear          = GearPosition::DRIVE;
    thief_driving.kl30_mv       = 12500;
    cluster.mcu_sim->setVehicleState(thief_driving);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::string immob_cmd = app::SvtOrchestrator::buildSignedCloudCommand(
        cluster.crypto_hal, cluster.cfg.cloud.command_hmac_secret, "SVT_IMMOBILIZE", 1001);
    TCU_REQUIRE(cluster.svtd->handleAuthenticatedSocCommand(immob_cmd));
    TCU_REQUIRE(cluster.svtd->currentState() == SvtState::IMMOBILIZE_PENDING);

    // Must NOT engage starter inhibit while vehicle is moving!
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    TCU_REQUIRE(cluster.svtd->currentState() == SvtState::IMMOBILIZE_PENDING);
    TCU_REQUIRE(cluster.mcu_sim->getVehicleState().starter_inhibit_active == false);

    // Replay of nonce 1001 must be rejected!
    TCU_REQUIRE(!cluster.svtd->handleAuthenticatedSocCommand(immob_cmd));

    // 5. Thief Stops Vehicle & Turns Ignition OFF -> Zero-Speed Interlock Engages Starter Inhibit!
    VehicleState thief_stopped = thief_driving;
    thief_stopped.speed_kmh     = 0.0;
    thief_stopped.ignition_kl15 = false;
    thief_stopped.gear          = GearPosition::PARK;
    cluster.mcu_sim->setVehicleState(thief_stopped);

    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.svtd->currentState() == SvtState::IMMOBILIZED &&
               cluster.mcu_sim->getVehicleState().starter_inhibit_active == true;
    }, 2500));

    // 6. Vehicle Recovered -> Authenticated Disarm Restores Normal Starter Operation
    std::string disarm_cmd = app::SvtOrchestrator::buildSignedCloudCommand(
        cluster.crypto_hal, cluster.cfg.cloud.command_hmac_secret, "SVT_DISARM_RECOVERED", 1002);
    TCU_REQUIRE(cluster.svtd->handleAuthenticatedSocCommand(disarm_cmd));
    TCU_REQUIRE(cluster.svtd->currentState() == SvtState::DISARMED);
    TCU_REQUIRE(cluster.mcu_sim->getVehicleState().starter_inhibit_active == false);

    std::cout << "[PASS] test_svt_workflow\n";
    return 0;
}

