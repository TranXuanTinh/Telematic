#include "common/msd_codec.h"
#include "test_cluster_harness.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace tcu;

    test::TcuTestCluster cluster("ecall");
    TCU_REQUIRE(cluster.startAll());
    TCU_REQUIRE(cluster.ecalld->currentState() == EcallState::IDLE_ARMED);

    // 1. Short accidental SOS button press (< 400ms debounce) MUST cancel without dialing 112
    cluster.ecalld->onSosButtonPressed();
    TCU_REQUIRE(cluster.ecalld->currentState() == EcallState::SOS_DEBOUNCE);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cluster.ecalld->onSosButtonReleased();
    TCU_REQUIRE(cluster.ecalld->currentState() == EcallState::IDLE_ARMED);
    TCU_REQUIRE(cluster.modem_hal.dialCount() == 0U);

    // 2. Severe Airbag Crash IRQ from Companion MCU -> < 100ms Preemption + 140-byte MSD + 112 Voice
    cluster.gnss_hal.setSimulatedPosition(21.028511, 105.804817, 92.0f, 22.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // Let GNSS trail populate

    CrashEvent crash{};
    crash.automatic_activation = true;
    crash.test_call            = false;
    crash.airbag_mask          = 0x03;
    crash.delta_v_x_kmh        = -56;
    crash.rollover             = true;
    cluster.mcu_sim->injectCrash(crash);

    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.ecalld->currentState() == EcallState::VOICE_ACTIVE;
    }, 2500));

    // Verify UN ECE R144 latency (< 100 ms trigger-to-dial preemption), 112 dial, and 140-byte MSD
    TCU_REQUIRE(cluster.ecalld->lastTriggerToDialLatencyMs() < 100U);
    TCU_REQUIRE(cluster.modem_hal.dialCount() == 1U);
    TCU_REQUIRE(cluster.modem_hal.lastDialedNumber() == "112");
    TCU_REQUIRE(!cluster.modem_hal.lastCallWasTest());
    TCU_REQUIRE(cluster.audio_hal.currentRoute() == AudioRoute::MODEM_VOICE_TO_CABIN);
    TCU_REQUIRE(cluster.mcu_sim->getEcallLedMode() == EcallLedMode::BLINK_CALL);

    auto raw_msd = cluster.modem_hal.lastSentMsd();
    TCU_REQUIRE(raw_msd.has_value());
    auto decoded = common::MsdCodec::decode(*raw_msd);
    TCU_REQUIRE(decoded.has_value());
    TCU_REQUIRE(decoded->vin == cluster.cfg.system.vin);
    TCU_REQUIRE(decoded->automatic_activation == true);
    TCU_REQUIRE(decoded->crash_delta_v_kmh == -56);
    TCU_REQUIRE(decoded->rollover_detected == true);
    TCU_REQUIRE(decoded->message_identifier == 1U);

    // 3. PSAP MSD Retransmission Request increments message_identifier -> 2
    TCU_REQUIRE(cluster.ecalld->retransmitMsd());
    auto raw_msd2 = cluster.modem_hal.lastSentMsd();
    TCU_REQUIRE(raw_msd2.has_value());
    auto decoded2 = common::MsdCodec::decode(*raw_msd2);
    TCU_REQUIRE(decoded2.has_value());
    TCU_REQUIRE(decoded2->message_identifier == 2U);

    // 4. PSAP Operator Hangup -> 60-minute Callback Standby + Auto-Answer on PSAP Ring
    cluster.ecalld->onPsapOperatorHangup();
    TCU_REQUIRE(cluster.ecalld->currentState() == EcallState::CALLBACK_STANDBY);
    TCU_REQUIRE(cluster.audio_hal.currentRoute() == AudioRoute::MUTED);

    cluster.modem_hal.simulateIncomingPsapCallback("+112000111");
    TCU_REQUIRE(test::waitUntil([&] {
        return cluster.ecalld->currentState() == EcallState::VOICE_ACTIVE &&
               cluster.audio_hal.currentRoute() == AudioRoute::MODEM_VOICE_TO_CABIN;
    }, 1500));

    cluster.ecalld->endCallbackStandbyWindow();
    TCU_REQUIRE(cluster.ecalld->currentState() == EcallState::IDLE_ARMED);

    std::cout << "[PASS] test_ecall_workflow (trigger-to-dial latency = "
              << cluster.ecalld->lastTriggerToDialLatencyMs() << " ms)\n";
    return 0;
}

