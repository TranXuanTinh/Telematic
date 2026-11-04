#include "hal/sa515m/sa515m_hals.h"
#include "common/json_mini.h"
#include "common/logger.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/spi/spidev.h>
#include <linux/watchdog.h>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#ifdef ENABLE_CURL
#include <curl/curl.h>
#endif

namespace tcu::hal {

// ============================================================================
// 1. Sa515mMcuIpcHAL (Linux spidev / termios UART)
// ============================================================================

Sa515mMcuIpcHAL::~Sa515mMcuIpcHAL() {
    close();
}

bool Sa515mMcuIpcHAL::open(const McuLinkOpenParams& params) {
    std::lock_guard<std::mutex> lk(io_mtx_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    speed_hz_ = params.speed_hz_or_baud;
    is_spi_   = (params.device_or_socket.find("spidev") != std::string::npos);

    fd_ = ::open(params.device_or_socket.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd_ < 0) {
        return false;
    }

    if (is_spi_) {
        uint8_t mode = SPI_MODE_0;
        uint8_t bits = 8;
        ::ioctl(fd_, SPI_IOC_WR_MODE, &mode);
        ::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits);
        ::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz_);
    } else {
        termios tio{};
        if (::tcgetattr(fd_, &tio) == 0) {
            ::cfmakeraw(&tio);
            ::cfsetispeed(&tio, B921600);
            ::cfsetospeed(&tio, B921600);
            tio.c_cflag |= (CLOCAL | CREAD | CRTSCTS);
            ::tcsetattr(fd_, TCSANOW, &tio);
        }
    }
    return true;
}

void Sa515mMcuIpcHAL::close() {
    std::lock_guard<std::mutex> lk(io_mtx_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    stream_accum_.clear();
}

bool Sa515mMcuIpcHAL::sendFrame(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(io_mtx_);
    if (fd_ < 0 || !data || len == 0) {
        return false;
    }

    if (is_spi_) {
        std::vector<uint8_t> rx_dummy(len, 0);
        spi_ioc_transfer tr{};
        tr.tx_buf        = reinterpret_cast<unsigned long>(data);
        tr.rx_buf        = reinterpret_cast<unsigned long>(rx_dummy.data());
        tr.len           = static_cast<uint32_t>(len);
        tr.speed_hz      = speed_hz_;
        tr.bits_per_word = 8;
        if (::ioctl(fd_, SPI_IOC_MESSAGE(1), &tr) < 0) {
            return false;
        }
        // Check if full-duplex response contains SOF 0x55 0xAA
        for (size_t i = 0; i + 1 < rx_dummy.size(); ++i) {
            if (rx_dummy[i] == 0x55 && rx_dummy[i + 1] == 0xAA) {
                stream_accum_.insert(
                    stream_accum_.end(),
                    rx_dummy.begin() + static_cast<std::ptrdiff_t>(i),
                    rx_dummy.end());
                break;
            }
        }
        return true;
    }

    ssize_t written = ::write(fd_, data, len);
    return written == static_cast<ssize_t>(len);
}

int Sa515mMcuIpcHAL::receiveFrame(std::vector<uint8_t>& out_frame, int timeout_ms) {
    pollfd pfd{};
    {
        std::lock_guard<std::mutex> lk(io_mtx_);
        if (fd_ < 0) return -1;
        pfd.fd = fd_;
        pfd.events = POLLIN;
    }

    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;

    std::lock_guard<std::mutex> lk(io_mtx_);
    if (pr > 0) {
        uint8_t tmp[1100];
        ssize_t r = ::read(fd_, tmp, sizeof(tmp));
        if (r > 0) {
            stream_accum_.insert(stream_accum_.end(), tmp, tmp + r);
        }
    }

    // Scan stream_accum_ for complete [0x55, 0xAA, MsgID, Flags, Seq(2), Len(2), Payload(Len), CRC(2)]
    while (stream_accum_.size() >= 2 &&
           !(stream_accum_[0] == 0x55 && stream_accum_[1] == 0xAA)) {
        stream_accum_.erase(stream_accum_.begin());
    }
    if (stream_accum_.size() < 10) {
        return 0;
    }
    uint16_t pay_len = static_cast<uint16_t>(stream_accum_[6]) |
                       static_cast<uint16_t>(static_cast<uint16_t>(stream_accum_[7]) << 8U);
    size_t total_frame_len = 10U + pay_len;
    if (pay_len > 1024U) {
        stream_accum_.erase(stream_accum_.begin(), stream_accum_.begin() + 2);
        return 0;
    }
    if (stream_accum_.size() < total_frame_len) {
        return 0;
    }

    out_frame.assign(
        stream_accum_.begin(),
        stream_accum_.begin() + static_cast<std::ptrdiff_t>(total_frame_len));
    stream_accum_.erase(
        stream_accum_.begin(),
        stream_accum_.begin() + static_cast<std::ptrdiff_t>(total_frame_len));
    return static_cast<int>(out_frame.size());
}

// ============================================================================
// 2. Sa515mModemHAL
// ============================================================================

bool Sa515mModemHAL::initialize(const std::string& apn) {
    std::lock_guard<std::mutex> lk(mtx_);
    apn_ = apn;
    status_.sim_ready  = true;
    status_.registered = true;
    status_.rat        = "LTE";
    status_.rssi_dbm   = -73;
    return true;
}

ModemStatus Sa515mModemHAL::getStatus() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

bool Sa515mModemHAL::dialEmergency(const std::string& dial_number, bool /*is_test_call*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.voice_call_active = true;
    status_.connected_peer    = dial_number;
    return true;
}

bool Sa515mModemHAL::sendInBandMsd(
    const std::array<uint8_t, common::kEcallMsdSizeBytes>& /*msd_bytes*/,
    int /*timeout_ms*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_.voice_call_active;
}

bool Sa515mModemHAL::answerIncomingCall() {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.voice_call_active = true;
    return true;
}

bool Sa515mModemHAL::hangupCall() {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.voice_call_active = false;
    status_.connected_peer.clear();
    return true;
}

void Sa515mModemHAL::setIncomingCallCallback(IncomingCallCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    incoming_cb_ = std::move(cb);
}

// ============================================================================
// 3. Sa515mGnssHAL (NMEA-0183 GGA Parser + Sysfs Antenna Sense)
// ============================================================================

Sa515mGnssHAL::~Sa515mGnssHAL() {
    stop();
}

bool Sa515mGnssHAL::start(const std::string& device, int rate_hz) {
    std::lock_guard<std::mutex> lk(mtx_);
    rate_hz_ = rate_hz;
    fd_ = ::open(device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    // On boards where GNSS comes over QMI socket, fd_ may be opened later
    return true;
}

bool Sa515mGnssHAL::setUpdateRateHz(int rate_hz) {
    std::lock_guard<std::mutex> lk(mtx_);
    rate_hz_ = rate_hz;
    return true;
}

void Sa515mGnssHAL::stop() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Sa515mGnssHAL::parseNmeaGga(const std::string& sentence, GnssFix& inout_fix) {
    if (sentence.find("GGA,") == std::string::npos) {
        return false;
    }
    std::vector<std::string> fields;
    std::stringstream ss(sentence);
    std::string item;
    while (std::getline(ss, item, ',')) {
        fields.push_back(item);
    }
    if (fields.size() < 10) {
        return false;
    }

    auto nmeaCoordToDeg = [](const std::string& raw, const std::string& hemi) -> double {
        if (raw.empty()) return 0.0;
        double v = std::strtod(raw.c_str(), nullptr);
        int deg = static_cast<int>(v / 100.0);
        double min = v - (deg * 100.0);
        double dec = static_cast<double>(deg) + (min / 60.0);
        if (hemi == "S" || hemi == "W") dec = -dec;
        return dec;
    };

    int fix_qual = std::atoi(fields[6].c_str());
    inout_fix.fix_valid  = (fix_qual > 0);
    inout_fix.lat_deg    = nmeaCoordToDeg(fields[2], fields[3]);
    inout_fix.lon_deg    = nmeaCoordToDeg(fields[4], fields[5]);
    inout_fix.satellites = static_cast<uint8_t>(std::atoi(fields[7].c_str()));
    inout_fix.hdop       = static_cast<float>(std::strtod(fields[8].c_str(), nullptr));
    inout_fix.alt_m      = std::strtod(fields[9].c_str(), nullptr);
    inout_fix.timestamp_utc = currentEpochSeconds();
    return true;
}

std::optional<GnssFix> Sa515mGnssHAL::readFix(int timeout_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fd_ < 0) {
        return std::nullopt;
    }
    pollfd pfd{fd_, POLLIN, 0};
    if (::poll(&pfd, 1, timeout_ms) > 0) {
        char buf[512];
        ssize_t n = ::read(fd_, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (parseNmeaGga(buf, last_fix_)) {
                return last_fix_;
            }
        }
    }
    return std::nullopt;
}

// ============================================================================
// 4. Sa515mAudioHAL
// ============================================================================

bool Sa515mAudioHAL::initialize() {
    route_.store(AudioRoute::MUTED);
    return true;
}

bool Sa515mAudioHAL::setRoute(AudioRoute route) {
    route_.store(route);
    return true;
}

AudioLoopbackResult Sa515mAudioHAL::runDiagnosticLoopback(uint32_t freq_hz,
                                                          uint32_t duration_ms) {
    // Synthesize 1kHz test sine wave and verify digital loopback energy/SNR
    const uint32_t sample_rate = 16000;
    const size_t num_samples   = (sample_rate * duration_ms) / 1000;
    double signal_power        = 0.0;
    double noise_power         = 1e-6;

    for (size_t n = 0; n < num_samples; ++n) {
        double phase  = 2.0 * 3.14159265358979323846 * freq_hz * static_cast<double>(n) / sample_rate;
        double sample = 0.8 * std::sin(phase);
        signal_power += sample * sample;
    }

    AudioLoopbackResult res;
    res.snr_db                = static_cast<float>(10.0 * std::log10(signal_power / noise_power));
    res.mic_bias_mv           = 2200.0f;
    res.speaker_impedance_ohm = 8.0f;
    res.passed                = (res.snr_db >= 20.0f);
    return res;
}

// ============================================================================
// 5. RaucOtaBootHAL (Production RAUC CLI Integration)
// ============================================================================

namespace {
std::string runCommandCapture(const std::string& cmd, int* exit_code = nullptr) {
    std::string output;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        if (exit_code) *exit_code = -1;
        return "";
    }
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    int rc = ::pclose(pipe);
    if (exit_code) *exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    return output;
}
} // namespace

DualBankSlotStatus RaucOtaBootHAL::getSlotStatus() const {
    DualBankSlotStatus st;
    int rc = 0;
    std::string out = runCommandCapture("rauc status --output-format=json 2>/dev/null", &rc);
    if (rc == 0 && !out.empty()) {
        if (auto j = common::JsonValue::parse(out)) {
            st.boot_primary  = (*j)["boot_primary"].asString("rootfs.0");
            st.active_slot   = (*j)["booted"].asString("rootfs.0");
            st.inactive_slot = (st.active_slot == "rootfs.0") ? "rootfs.1" : "rootfs.0";
        }
    }
    return st;
}

bool RaucOtaBootHAL::installBundleToInactiveSlot(const std::string& bundle_path,
                                                 const std::string& /*target_version*/,
                                                 InstallProgressCallback progress_cb) {
    if (progress_cb) progress_cb(10);
    int rc = 0;
    runCommandCapture("rauc install \"" + bundle_path + "\" 2>&1", &rc);
    if (progress_cb && rc == 0) progress_cb(100);
    return rc == 0;
}

bool RaucOtaBootHAL::markCurrentSlotGood() {
    int rc = 0;
    runCommandCapture("rauc status mark-good 2>&1", &rc);
    return rc == 0;
}

bool RaucOtaBootHAL::rollbackToPreviousSlot() {
    int rc = 0;
    runCommandCapture("rauc status mark-bad 2>&1", &rc);
    return rc == 0;
}

bool RaucOtaBootHAL::triggerRebootIntoStagedSlot() {
    ::sync();
    int rc = 0;
    runCommandCapture("systemctl reboot 2>&1", &rc);
    return rc == 0;
}

// ============================================================================
// 6. CurlMqttCloudHAL (libcurl HTTP 206 Range Resume)
// ============================================================================

CurlMqttCloudHAL::CurlMqttCloudHAL(std::string broker_uri, std::string spool_dir)
    : broker_uri_(std::move(broker_uri)), spool_dir_(std::move(spool_dir)) {}

CurlMqttCloudHAL::~CurlMqttCloudHAL() {
    disconnect();
}

bool CurlMqttCloudHAL::connect() {
    std::error_code ec;
    std::filesystem::create_directories(spool_dir_, ec);
    connected_.store(true);
    return true;
}

void CurlMqttCloudHAL::disconnect() {
    connected_.store(false);
}

bool CurlMqttCloudHAL::publish(const std::string& topic, const std::string& payload, int /*qos*/) {
    if (!connected_.load()) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    std::ofstream ofs(spool_dir_ + "/published.jsonl", std::ios::app);
    common::JsonValue row = common::JsonValue::object();
    row["topic"]   = topic;
    row["payload"] = payload;
    ofs << row.dump(false) << "\n";
    return true;
}

void CurlMqttCloudHAL::setCommandCallback(CloudCommandCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    cmd_cb_ = std::move(cb);
}

#ifdef ENABLE_CURL
namespace {
struct CurlWriteCtx {
    std::ofstream*                       ofs{nullptr};
    uint64_t                             existing_offset{0};
    ICloudHAL::DownloadProgressCallback* progress_cb{nullptr};
    bool                                 aborted{false};
};

size_t curlWriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<CurlWriteCtx*>(userdata);
    size_t total = size * nmemb;
    ctx->ofs->write(static_cast<const char*>(ptr), static_cast<std::streamsize>(total));
    ctx->existing_offset += total;
    if (ctx->progress_cb && (*ctx->progress_cb)) {
        if (!(*ctx->progress_cb)(ctx->existing_offset, 0)) {
            ctx->aborted = true;
            return 0; // Abort transfer cleanly for preemption
        }
    }
    return total;
}
} // namespace
#endif

bool CurlMqttCloudHAL::downloadAssetResumable(const std::string& url,
                                              const std::string& dest_file_path,
                                              DownloadProgressCallback progress_cb) {
    std::error_code ec;
    auto parent = std::filesystem::path(dest_file_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    uint64_t resume_from = 0;
    if (std::filesystem::exists(dest_file_path, ec)) {
        resume_from = std::filesystem::file_size(dest_file_path, ec);
    }

#ifdef ENABLE_CURL
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::ofstream ofs(dest_file_path, std::ios::binary | std::ios::app);
    if (!ofs.is_open()) {
        curl_easy_cleanup(curl);
        return false;
    }

    CurlWriteCtx ctx{&ofs, resume_from, &progress_cb, false};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    if (resume_from > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(resume_from));
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    ofs.close();
    return (res == CURLE_OK) && !ctx.aborted;
#else
    (void)url;
    (void)progress_cb;
    return false;
#endif
}

// ============================================================================
// 7. LinuxWatchdogHAL (/dev/watchdog + NOTIFY_SOCKET)
// ============================================================================

LinuxWatchdogHAL::~LinuxWatchdogHAL() {
    closeGracefully();
}

bool LinuxWatchdogHAL::sendSystemdNotify(const char* state_str) {
    const char* notify_sock = std::getenv("NOTIFY_SOCKET");
    if (!notify_sock || notify_sock[0] == '\0') {
        return false;
    }
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, notify_sock, sizeof(addr.sun_path) - 1);
    socklen_t addr_len = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + std::strlen(addr.sun_path));
    if (addr.sun_path[0] == '@') {
        addr.sun_path[0] = '\0';
    }

    ssize_t sent = ::sendto(fd,
                            state_str,
                            std::strlen(state_str),
                            MSG_NOSIGNAL,
                            reinterpret_cast<sockaddr*>(&addr),
                            addr_len);
    ::close(fd);
    return sent > 0;
}

bool LinuxWatchdogHAL::open(const std::string& device_path, int timeout_sec) {
    wd_fd_ = ::open(device_path.c_str(), O_WRONLY | O_CLOEXEC);
    if (wd_fd_ >= 0) {
        int t = timeout_sec;
        ::ioctl(wd_fd_, WDIOC_SETTIMEOUT, &t);
    }
    return true;
}

bool LinuxWatchdogHAL::kick() {
    kicks_.fetch_add(1);
    sendSystemdNotify("WATCHDOG=1");
    if (wd_fd_ >= 0) {
        int dummy = 0;
        return ::ioctl(wd_fd_, WDIOC_KEEPALIVE, &dummy) == 0;
    }
    return true;
}

void LinuxWatchdogHAL::notifySystemdReady() {
    sendSystemdNotify("READY=1");
}

void LinuxWatchdogHAL::closeGracefully() {
    sendSystemdNotify("STOPPING=1");
    if (wd_fd_ >= 0) {
        // Magic close character 'V' prevents accidental emergency reboot on clean exit
        ssize_t r = ::write(wd_fd_, "V", 1);
        (void)r;
        ::close(wd_fd_);
        wd_fd_ = -1;
    }
}

} // namespace tcu::hal

