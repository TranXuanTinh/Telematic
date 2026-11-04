#pragma once

#include "hal/i_audio_hal.h"
#include "hal/i_cloud_hal.h"
#include "hal/i_gnss_hal.h"
#include "hal/i_mcu_ipc_hal.h"
#include "hal/i_modem_hal.h"
#include "hal/i_ota_boot_hal.h"
#include "hal/i_watchdog_hal.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tcu::hal {

/// Host PC & QEMU Mock MCU Transport over AF_UNIX SOCK_SEQPACKET to tcu-mcu-sim.
class MockMcuIpcHAL : public IMcuIpcHAL {
public:
    MockMcuIpcHAL() = default;
    ~MockMcuIpcHAL() override;

    bool open(const McuLinkOpenParams& params) override;
    void close() override;
    bool isOpen() const override { return sock_fd_ >= 0; }
    bool sendFrame(const uint8_t* data, size_t len) override;
    int receiveFrame(std::vector<uint8_t>& out_frame, int timeout_ms) override;

private:
    int        sock_fd_{-1};
    std::mutex io_mtx_;
};

/// Deterministic Mock Cellular 4G/5G VoLTE Modem for Host & CI Testing.
class MockModemHAL : public IModemHAL {
public:
    MockModemHAL();

    bool initialize(const std::string& apn) override;
    ModemStatus getStatus() const override;
    bool dialEmergency(const std::string& dial_number, bool is_test_call) override;
    bool sendInBandMsd(
        const std::array<uint8_t, common::kEcallMsdSizeBytes>& msd_bytes,
        int timeout_ms) override;
    bool answerIncomingCall() override;
    bool hangupCall() override;
    void setIncomingCallCallback(IncomingCallCallback cb) override;

    // Test & Fault Injection Controls
    void setNetworkHealth(bool sim_ready, bool registered, int16_t rssi_dbm);
    void setMsdAckBehavior(bool should_ack);
    void simulateIncomingPsapCallback(const std::string& psap_number = "+112000999");

    uint32_t dialCount() const;
    std::string lastDialedNumber() const;
    bool lastCallWasTest() const;
    std::optional<std::array<uint8_t, common::kEcallMsdSizeBytes>> lastSentMsd() const;

private:
    mutable std::mutex   mtx_;
    ModemStatus          status_{};
    bool                 msd_should_ack_{true};
    uint32_t             dial_count_{0};
    std::string          last_dialed_;
    bool                 last_was_test_{false};
    std::optional<std::array<uint8_t, common::kEcallMsdSizeBytes>> last_msd_;
    IncomingCallCallback incoming_cb_;
};

/// Deterministic Mock Multi-Constellation GNSS & Active Antenna Sensor.
class MockGnssHAL : public IGnssHAL {
public:
    explicit MockGnssHAL(double init_lat = 21.028511, double init_lon = 105.804817);

    bool start(const std::string& device, int rate_hz) override;
    bool setUpdateRateHz(int rate_hz) override;
    void stop() override;
    std::optional<GnssFix> readFix(int timeout_ms) override;

    // Test & Fault Injection Controls
    void setSimulatedPosition(double lat_deg, double lon_deg, float heading_deg = 90.0f,
                              float speed_mps = 0.0f);
    void setAntennaFault(AntennaHealth health, float current_ma);
    int currentRateHz() const;

private:
    mutable std::mutex mtx_;
    bool               started_{false};
    int                rate_hz_{1};
    GnssFix            fix_{};
};

/// Deterministic Mock Cabin Voice Audio Codec & Loopback BIST.
class MockAudioHAL : public IAudioHAL {
public:
    bool initialize() override;
    bool setRoute(AudioRoute route) override;
    AudioRoute currentRoute() const override { return route_.load(); }
    AudioLoopbackResult runDiagnosticLoopback(uint32_t freq_hz,
                                              uint32_t duration_ms) override;

    void setLoopbackHealthy(bool healthy) { loopback_healthy_.store(healthy); }

private:
    std::atomic<AudioRoute> route_{AudioRoute::MUTED};
    std::atomic<bool>       loopback_healthy_{true};
};

/// JSON-Persisted Dual-Bank A/B RootFS Slot Simulator (Models RAUC + Bootloader).
class MockOtaBootHAL : public IOtaBootHAL {
public:
    explicit MockOtaBootHAL(std::string state_file_path,
                            std::string initial_version = "2.3.0");

    DualBankSlotStatus getSlotStatus() const override;
    bool installBundleToInactiveSlot(const std::string& bundle_path,
                                     const std::string& target_version,
                                     InstallProgressCallback progress_cb = nullptr) override;
    bool markCurrentSlotGood() override;
    bool rollbackToPreviousSlot() override;
    bool triggerRebootIntoStagedSlot() override;

private:
    void loadLocked();
    void saveLocked() const;

    mutable std::mutex mtx_;
    std::string        state_file_;
    DualBankSlotStatus status_{};
    std::string        previous_version_{"2.3.0"};
    std::string        staged_version_{"2.3.0"};
};

/// Deterministic Mock Cloud Transport (mTLS MQTT Spool + Resumable Chunk Downloader).
class MockCloudHAL : public ICloudHAL {
public:
    explicit MockCloudHAL(std::string spool_dir = "/tmp/tcu/cloud_spool");

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override { return connected_.load(); }
    bool publish(const std::string& topic, const std::string& payload, int qos = 1) override;
    void setCommandCallback(CloudCommandCallback cb) override;
    bool downloadAssetResumable(const std::string& url,
                                const std::string& dest_file_path,
                                DownloadProgressCallback progress_cb = nullptr) override;

    // Test & Simulation Controls
    void setOnline(bool online) { connected_.store(online); }
    void injectIncomingCommand(const std::string& topic, const std::string& payload);
    std::vector<std::pair<std::string, std::string>> publishedMessages() const;
    void clearPublishedMessages();

private:
    std::string                                      spool_dir_;
    std::atomic<bool>                                connected_{true};
    mutable std::mutex                               mtx_;
    CloudCommandCallback                             cmd_cb_;
    std::vector<std::pair<std::string, std::string>> published_;
};

/// Non-Destructive Host Watchdog HAL.
class MockWatchdogHAL : public IWatchdogHAL {
public:
    bool open(const std::string& device_path, int timeout_sec) override;
    bool kick() override;
    void notifySystemdReady() override;
    void closeGracefully() override;
    uint64_t kickCount() const override { return kicks_.load(); }

private:
    std::atomic<uint64_t> kicks_{0};
};

} // namespace tcu::hal

