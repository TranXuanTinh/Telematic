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
#include <string>
#include <vector>

namespace tcu::hal {

/// Production Linux SPI (/dev/spidevX.Y) & High-Speed UART (/dev/ttyHSX) MCU Link HAL.
class Sa515mMcuIpcHAL : public IMcuIpcHAL {
public:
    Sa515mMcuIpcHAL() = default;
    ~Sa515mMcuIpcHAL() override;

    bool open(const McuLinkOpenParams& params) override;
    void close() override;
    bool isOpen() const override { return fd_ >= 0; }
    bool sendFrame(const uint8_t* data, size_t len) override;
    int receiveFrame(std::vector<uint8_t>& out_frame, int timeout_ms) override;

private:
    int               fd_{-1};
    bool              is_spi_{false};
    uint32_t          speed_hz_{10000000};
    std::mutex        io_mtx_;
    std::vector<uint8_t> stream_accum_;
};

/// Production Cellular 4G/5G Modem HAL (ModemManager / QMI / AT Emergency Voice).
class Sa515mModemHAL : public IModemHAL {
public:
    bool initialize(const std::string& apn) override;
    ModemStatus getStatus() const override;
    bool dialEmergency(const std::string& dial_number, bool is_test_call) override;
    bool sendInBandMsd(
        const std::array<uint8_t, common::kEcallMsdSizeBytes>& msd_bytes,
        int timeout_ms) override;
    bool answerIncomingCall() override;
    bool hangupCall() override;
    void setIncomingCallCallback(IncomingCallCallback cb) override;

private:
    mutable std::mutex   mtx_;
    std::string          apn_;
    ModemStatus          status_{};
    IncomingCallCallback incoming_cb_;
};

/// Production Multi-Constellation GNSS HAL (NMEA-0183 Stream + Active LNA Current Sense).
class Sa515mGnssHAL : public IGnssHAL {
public:
    ~Sa515mGnssHAL() override;
    bool start(const std::string& device, int rate_hz) override;
    bool setUpdateRateHz(int rate_hz) override;
    void stop() override;
    std::optional<GnssFix> readFix(int timeout_ms) override;

    /// Parse an NMEA-0183 $GPGGA or $GNGGA sentence into GnssFix.
    static bool parseNmeaGga(const std::string& sentence, GnssFix& inout_fix);

private:
    int        fd_{-1};
    int        rate_hz_{1};
    GnssFix    last_fix_{};
    std::mutex mtx_;
};

/// Production Cabin Voice Audio Codec HAL (ALSA Mixer + Digital Sine Loopback SNR).
class Sa515mAudioHAL : public IAudioHAL {
public:
    bool initialize() override;
    bool setRoute(AudioRoute route) override;
    AudioRoute currentRoute() const override { return route_.load(); }
    AudioLoopbackResult runDiagnosticLoopback(uint32_t freq_hz,
                                              uint32_t duration_ms) override;

private:
    std::atomic<AudioRoute> route_{AudioRoute::MUTED};
};

/// Production RAUC Dual-Bank A/B RootFS Bootloader HAL.
class RaucOtaBootHAL : public IOtaBootHAL {
public:
    DualBankSlotStatus getSlotStatus() const override;
    bool installBundleToInactiveSlot(const std::string& bundle_path,
                                     const std::string& target_version,
                                     InstallProgressCallback progress_cb = nullptr) override;
    bool markCurrentSlotGood() override;
    bool rollbackToPreviousSlot() override;
    bool triggerRebootIntoStagedSlot() override;
};

/// Production HTTPS Range (libcurl) + mTLS Cloud Transport HAL.
class CurlMqttCloudHAL : public ICloudHAL {
public:
    explicit CurlMqttCloudHAL(std::string broker_uri, std::string spool_dir);
    ~CurlMqttCloudHAL() override;

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override { return connected_.load(); }
    bool publish(const std::string& topic, const std::string& payload, int qos = 1) override;
    void setCommandCallback(CloudCommandCallback cb) override;
    bool downloadAssetResumable(const std::string& url,
                                const std::string& dest_file_path,
                                DownloadProgressCallback progress_cb = nullptr) override;

private:
    std::string          broker_uri_;
    std::string          spool_dir_;
    std::atomic<bool>    connected_{false};
    std::mutex           mtx_;
    CloudCommandCallback cmd_cb_;
};

/// Production Linux /dev/watchdog + systemd sd_notify Watchdog HAL.
class LinuxWatchdogHAL : public IWatchdogHAL {
public:
    ~LinuxWatchdogHAL() override;
    bool open(const std::string& device_path, int timeout_sec) override;
    bool kick() override;
    void notifySystemdReady() override;
    void closeGracefully() override;
    uint64_t kickCount() const override { return kicks_.load(); }

    static bool sendSystemdNotify(const char* state_str);

private:
    int                   wd_fd_{-1};
    std::atomic<uint64_t> kicks_{0};
};

} // namespace tcu::hal

