#include "test_cluster_harness.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
    using namespace tcu;

    test::TcuTestCluster cluster("diag");
    TCU_REQUIRE(cluster.startAll());

    // 1. Healthy Startup BIST -> All Subsystems OK, 0 DTCs, Cabin LED GREEN_READY
    auto b0 = cluster.diagd->runFullBist();
    TCU_REQUIRE(b0.allCriticalOk());
    TCU_REQUIRE(b0.active_dtc_count == 0U);
    TCU_REQUIRE(cluster.mcu_sim->getEcallLedMode() == EcallLedMode::GREEN_READY);

    // 2. Fault Injection: GNSS Antenna Open Circuit + Audio Codec Failure -> DTCs + RED_FAULT LED
    cluster.gnss_hal.setAntennaFault(AntennaHealth::OPEN_CIRCUIT, 0.1f);
    cluster.audio_hal.setLoopbackHealthy(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // Allow GNSS poll to propagate

    auto b1 = cluster.diagd->runFullBist();
    TCU_REQUIRE(!b1.gnss_antenna_ok);
    TCU_REQUIRE(!b1.audio_loopback_ok);
    TCU_REQUIRE(!b1.allCriticalOk());
    TCU_REQUIRE(b1.active_dtc_count >= 2U);
    TCU_REQUIRE(cluster.mcu_sim->getEcallLedMode() == EcallLedMode::RED_FAULT);

    // 3. ISO 14229 UDS Service 0x22 (ReadDataByIdentifier): VIN (0xF190) & BUB SoC/SoH (0xFD03)
    auto vin_resp = cluster.diagd->processUdsRequest({0x22, 0xF1, 0x90});
    TCU_REQUIRE(vin_resp.size() == 3U + 17U);
    TCU_REQUIRE(vin_resp[0] == 0x62U && vin_resp[1] == 0xF1U && vin_resp[2] == 0x90U);
    std::string read_vin(vin_resp.begin() + 3, vin_resp.end());
    TCU_REQUIRE(read_vin == "VN1TCU515M0000001");

    auto bub_resp = cluster.diagd->processUdsRequest({0x22, 0xFD, 0x03});
    TCU_REQUIRE(bub_resp.size() == 9U && bub_resp[0] == 0x62U);

    // 4. UDS Service 0x2E (WriteDataByIdentifier): Blocked in Default Session (NRC 0x33), Allowed in EOL (0x10 0x60)
    std::string new_vin = "VN1TCU515M9999999";
    std::vector<uint8_t> write_vin_req = {0x2E, 0xF1, 0x90};
    write_vin_req.insert(write_vin_req.end(), new_vin.begin(), new_vin.end());

    auto denied = cluster.diagd->processUdsRequest(write_vin_req);
    TCU_REQUIRE(denied.size() == 3U && denied[0] == 0x7FU && denied[2] == 0x33U);

    auto prog_sess = cluster.diagd->processUdsRequest({0x10, 0x60});
    TCU_REQUIRE(!prog_sess.empty() && prog_sess[0] == 0x50U);

    auto write_ok = cluster.diagd->processUdsRequest(write_vin_req);
    TCU_REQUIRE(write_ok.size() == 3U && write_ok[0] == 0x6EU);

    auto vin_verify = cluster.diagd->processUdsRequest({0x22, 0xF1, 0x90});
    TCU_REQUIRE(std::string(vin_verify.begin() + 3, vin_verify.end()) == new_vin);

    // 5. UDS Service 0x19 (ReadDTCInformation) & 0x14 (ClearDiagnosticInformation)
    auto dtc_resp = cluster.diagd->processUdsRequest({0x19, 0x02, 0x09});
    // Header (3 bytes: 59 02 09) + 2 DTC entries * 4 bytes = 11 bytes
    TCU_REQUIRE(dtc_resp.size() >= 11U && dtc_resp[0] == 0x59U);

    // Restore hardware health, switch to Extended Session (0x10 0x03), clear DTCs, re-run BIST
    cluster.gnss_hal.setAntennaFault(AntennaHealth::OK, 18.5f);
    cluster.audio_hal.setLoopbackHealthy(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    TCU_REQUIRE(cluster.diagd->processUdsRequest({0x10, 0x03})[0] == 0x50U);
    auto clr_resp = cluster.diagd->processUdsRequest({0x14, 0xFF, 0xFF, 0xFF});
    TCU_REQUIRE(!clr_resp.empty() && clr_resp[0] == 0x54U);

    auto b2 = cluster.diagd->runFullBist();
    TCU_REQUIRE(b2.allCriticalOk());
    TCU_REQUIRE(b2.active_dtc_count == 0U);
    TCU_REQUIRE(cluster.mcu_sim->getEcallLedMode() == EcallLedMode::GREEN_READY);

    // 6. UDS Service 0x31 RoutineControl (0x0201 = Non-Emergency Test eCall)
    auto rc_resp = cluster.diagd->processUdsRequest({0x31, 0x01, 0x02, 0x01});
    TCU_REQUIRE(rc_resp.size() >= 5U && rc_resp[0] == 0x71U);

    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.modem_hal.dialCount() >= 1U && cluster.modem_hal.lastCallWasTest();
    }, 2000));
    TCU_REQUIRE(cluster.modem_hal.lastDialedNumber() == cluster.cfg.modem.test_psap_number);

    cluster.ecalld->onPsapOperatorHangup();
    TCU_REQUIRE(cluster.ecalld->currentState() == EcallState::IDLE_ARMED);

    std::cout << "[PASS] test_diag_and_uds\n";
    return 0;
}
