#pragma once

#include "common/msd_codec.h"
#include "common/types.h"
#include "ipc/ipc_client.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace tcu::app {

/// Tier 1 Feature Orchestrator for tcu-ecalld (CEN EN 15722 / UN ECE R144).
/// Runs with highest real-time priority (SCHED_FIFO 80 when privileged).
class EcallOrchestrator {
public:
    explicit EcallOrchestrator(const TcuConfig& cfg);
    ~EcallOrchestrator();

    bool start();
    void stop();

    EcallState currentState() const;
    std::optional<common::EcallMsdPayload> lastDecodedMsd() const;
    uint32_t completedEcallCount() const noexcept { return completed_calls_.load(); }
    uint64_t lastTriggerToDialLatencyMs() const noexcept { return last_dial_latency_ms_.load(); }

    // Manual SOS push-button debounce & PSAP call lifecycle controls
    void onSosButtonPressed();
    void onSosButtonReleased();
    void triggerEmergency(const CrashEvent& crash);
    bool retransmitMsd();
    void onPsapOperatorHangup();
    void endCallbackStandbyWindow();

private:
    void registerHandlers();
    void fsmWorkerLoop();
    void executeEmergencySession(const CrashEvent& crash);
    GnssTrail fetchGnssTrailViaIpc();

    TcuConfig                               cfg_;
    ipc::IpcClient                          ipc_;

    std::atomic<bool>                       running_{false};
    std::thread                             fsm_thread_;

    mutable std::mutex                      mtx_;
    std::condition_variable                 cv_;
    EcallState                              state_{EcallState::IDLE_ARMED};
    bool                                    sos_held_{false};
    uint64_t                                sos_press_start_ms_{0};
    std::optional<CrashEvent>               pending_crash_;
    CrashEvent                              active_crash_{};
    uint8_t                                 msd_seq_id_{1};
    uint64_t                                callback_deadline_ms_{0};
    std::optional<common::EcallMsdPayload>  last_msd_;

    std::atomic<uint32_t>                   completed_calls_{0};
    std::atomic<uint64_t>                   last_dial_latency_ms_{0};
};

} // namespace tcu::app

