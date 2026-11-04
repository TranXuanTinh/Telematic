#include "hal/mock/mock_hals.h"
#include "common/json_mini.h"
#include "hal/sa515m/sa515m_hals.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace tcu::hal {

// ============================================================================
// 1. MockMcuIpcHAL
// ============================================================================

MockMcuIpcHAL::~MockMcuIpcHAL() {
    close();
}

bool MockMcuIpcHAL::open(const McuLinkOpenParams& params) {
    std::lock_guard<std::mutex> lk(io_mtx_);
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (true) {
        int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, params.device_or_socket.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            sock_fd_ = fd;
            return true;
        }
        ::close(fd);
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void MockMcuIpcHAL::close() {
    std::lock_guard<std::mutex> lk(io_mtx_);
    if (sock_fd_ >= 0) {
        ::shutdown(sock_fd_, SHUT_RDWR);
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
}

bool MockMcuIpcHAL::sendFrame(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(io_mtx_);
    if (sock_fd_ < 0 || !data || len == 0) return false;
    ssize_t sent = ::send(sock_fd_, data, len, MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(len);
}

int MockMcuIpcHAL::receiveFrame(std::vector<uint8_t>& out_frame, int timeout_ms) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(io_mtx_);
        fd = sock_fd_;
    }
    if (fd < 0) return -1;

    pollfd pfd{fd, POLLIN, 0};
    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr == 0) return 0;
    if (pr < 0)  return -1;

    uint8_t buf[2048];
    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0) return -1;
    out_frame.assign(buf, buf + r);
    return static_cast<int>(r);
}

// ============================================================================
// 2. MockModemHAL
// ============================================================================

MockModemHAL::MockModemHAL() {
    status_.sim_ready            = true;
    status_.registered           = true;
    status_.rat                  = "LTE-VoLTE";
    status_.rssi_dbm             = -72;
    status_.serving_cell         = CellTowerInfo{452, 4, 1020, 8841201, -78};
    status_.neighbor_cells       = {CellTowerInfo{452, 4, 1020, 8841202, -85},
                                    CellTowerInfo{452, 4, 1021, 8841305, -91}};
}

bool MockModemHAL::initialize(const std::string& /*apn*/) {
    return true;
}

ModemStatus MockModemHAL::getStatus() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

bool MockModemHAL::dialEmergency(const std::string& dial_number, bool is_test_call) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Live 112 eCall works even without normal home registration (Emergency Attach per TS 122.101)
    if (is_test_call && (!status_.sim_ready || !status_.registered)) {
        return false;
    }
    status_.voice_call_active = true;
    status_.connected_peer    = dial_number;
    last_dialed_              = dial_number;
    last_was_test_            = is_test_call;
    ++dial_count_;
    return true;
}

bool MockModemHAL::sendInBandMsd(
    const std::array<uint8_t, common::kEcallMsdSizeBytes>& msd_bytes,
    int /*timeout_ms*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!status_.voice_call_active) {
        return false;
    }
    last_msd_ = msd_bytes;
    return msd_should_ack_;
}

bool MockModemHAL::answerIncomingCall() {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.voice_call_active = true;
    return true;
}

bool MockModemHAL::hangupCall() {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.voice_call_active = false;
    status_.connected_peer.clear();
    return true;
}

void MockModemHAL::setIncomingCallCallback(IncomingCallCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    incoming_cb_ = std::move(cb);
}

void MockModemHAL::setNetworkHealth(bool sim_ready, bool registered, int16_t rssi_dbm) {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.sim_ready  = sim_ready;
    status_.registered = registered;
    status_.rssi_dbm   = rssi_dbm;
}

void MockModemHAL::setMsdAckBehavior(bool should_ack) {
    std::lock_guard<std::mutex> lk(mtx_);
    msd_should_ack_ = should_ack;
}

void MockModemHAL::simulateIncomingPsapCallback(const std::string& psap_number) {
    IncomingCallCallback cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        status_.connected_peer = psap_number;
        cb = incoming_cb_;
    }
    if (cb) {
        cb(psap_number);
    }
}

uint32_t MockModemHAL::dialCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return dial_count_;
}

std::string MockModemHAL::lastDialedNumber() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_dialed_;
}

bool MockModemHAL::lastCallWasTest() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_was_test_;
}

std::optional<std::array<uint8_t, common::kEcallMsdSizeBytes>> MockModemHAL::lastSentMsd() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_msd_;
}

// ============================================================================
// 3. MockGnssHAL
// ============================================================================

MockGnssHAL::MockGnssHAL(double init_lat, double init_lon) {
    fix_.lat_deg            = init_lat;
    fix_.lon_deg            = init_lon;
    fix_.alt_m              = 14.5;
    fix_.heading_deg        = 84.0f;
    fix_.speed_mps          = 0.0f;
    fix_.hdop               = 0.8f;
    fix_.satellites         = 12;
    fix_.fix_valid          = true;
    fix_.antenna_health     = AntennaHealth::OK;
    fix_.antenna_current_ma = 18.0f;
    fix_.timestamp_utc      = currentEpochSeconds();
}

bool MockGnssHAL::start(const std::string& /*device*/, int rate_hz) {
    std::lock_guard<std::mutex> lk(mtx_);
    started_ = true;
    rate_hz_ = (rate_hz > 0) ? rate_hz : 1;
    return true;
}

bool MockGnssHAL::setUpdateRateHz(int rate_hz) {
    std::lock_guard<std::mutex> lk(mtx_);
    rate_hz_ = (rate_hz > 0) ? rate_hz : 1;
    return true;
}

void MockGnssHAL::stop() {
    std::lock_guard<std::mutex> lk(mtx_);
    started_ = false;
}

std::optional<GnssFix> MockGnssHAL::readFix(int timeout_ms) {
    if (timeout_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 15)));
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!started_) return std::nullopt;
    fix_.timestamp_utc = currentEpochSeconds();
    return fix_;
}

void MockGnssHAL::setSimulatedPosition(double lat_deg, double lon_deg,
                                       float heading_deg, float speed_mps) {
    std::lock_guard<std::mutex> lk(mtx_);
    fix_.lat_deg       = lat_deg;
    fix_.lon_deg       = lon_deg;
    fix_.heading_deg   = heading_deg;
    fix_.speed_mps     = speed_mps;
    fix_.fix_valid     = (fix_.antenna_health == AntennaHealth::OK);
    fix_.timestamp_utc = currentEpochSeconds();
}

void MockGnssHAL::setAntennaFault(AntennaHealth health, float current_ma) {
    std::lock_guard<std::mutex> lk(mtx_);
    fix_.antenna_health     = health;
    fix_.antenna_current_ma = current_ma;
    fix_.fix_valid          = (health == AntennaHealth::OK);
}

int MockGnssHAL::currentRateHz() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return rate_hz_;
}

// ============================================================================
// 4. MockAudioHAL
// ============================================================================

bool MockAudioHAL::initialize() {
    route_.store(AudioRoute::MUTED);
    return true;
}

bool MockAudioHAL::setRoute(AudioRoute route) {
    route_.store(route);
    return true;
}

AudioLoopbackResult MockAudioHAL::runDiagnosticLoopback(uint32_t /*freq_hz*/,
                                                        uint32_t /*duration_ms*/) {
    AudioLoopbackResult res;
    if (loopback_healthy_.load()) {
        res.passed                = true;
        res.snr_db                = 41.2f;
        res.mic_bias_mv           = 2200.0f;
        res.speaker_impedance_ohm = 8.0f;
    } else {
        res.passed                = false;
        res.snr_db                = 3.1f;
        res.mic_bias_mv           = 120.0f;
        res.speaker_impedance_ohm = 999.0f; // Open speaker coil
    }
    return res;
}

// ============================================================================
// 5. MockOtaBootHAL (Persistent Dual-Bank A/B Slot State Machine)
// ============================================================================

MockOtaBootHAL::MockOtaBootHAL(std::string state_file_path, std::string initial_version)
    : state_file_(std::move(state_file_path)),
      previous_version_(initial_version),
      staged_version_(initial_version) {
    status_.installed_version = std::move(initial_version);
    std::lock_guard<std::mutex> lk(mtx_);
    loadLocked();
}

void MockOtaBootHAL::loadLocked() {
    if (state_file_.empty() || !std::filesystem::exists(state_file_)) {
        saveLocked();
        return;
    }
    std::ifstream ifs(state_file_);
    std::stringstream ss;
    ss << ifs.rdbuf();
    if (auto j = common::JsonValue::parse(ss.str())) {
        status_.active_slot              = (*j)["active_slot"].asString("rootfs.0");
        status_.inactive_slot            = (*j)["inactive_slot"].asString("rootfs.1");
        status_.boot_primary             = (*j)["boot_primary"].asString("rootfs.0");
        status_.active_marked_good       = (*j)["active_marked_good"].asBool(true);
        status_.pending_post_boot_verify = (*j)["pending_post_boot_verify"].asBool(false);
        status_.rollback_count           = static_cast<uint32_t>((*j)["rollback_count"].asUint64(0));
        status_.installed_version        = (*j)["installed_version"].asString("2.3.0");
        previous_version_                = (*j)["previous_version"].asString(status_.installed_version);
        staged_version_                  = (*j)["staged_version"].asString(status_.installed_version);
    }
}

void MockOtaBootHAL::saveLocked() const {
    if (state_file_.empty()) return;
    std::error_code ec;
    auto parent = std::filesystem::path(state_file_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    common::JsonValue j = common::JsonValue::object();
    j["active_slot"]              = status_.active_slot;
    j["inactive_slot"]            = status_.inactive_slot;
    j["boot_primary"]             = status_.boot_primary;
    j["active_marked_good"]       = status_.active_marked_good;
    j["pending_post_boot_verify"] = status_.pending_post_boot_verify;
    j["rollback_count"]           = status_.rollback_count;
    j["installed_version"]        = status_.installed_version;
    j["previous_version"]         = previous_version_;
    j["staged_version"]           = staged_version_;

    std::ofstream ofs(state_file_, std::ios::trunc);
    ofs << j.dump(true);
}

DualBankSlotStatus MockOtaBootHAL::getSlotStatus() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

bool MockOtaBootHAL::installBundleToInactiveSlot(const std::string& bundle_path,
                                                 const std::string& target_version,
                                                 InstallProgressCallback progress_cb) {
    if (!std::filesystem::exists(bundle_path)) {
        return false;
    }
    if (progress_cb) progress_cb(25);
    if (progress_cb) progress_cb(75);

    std::lock_guard<std::mutex> lk(mtx_);
    previous_version_   = status_.installed_version;
    staged_version_     = target_version;
    status_.boot_primary = status_.inactive_slot;
    saveLocked();

    if (progress_cb) progress_cb(100);
    return true;
}

bool MockOtaBootHAL::triggerRebootIntoStagedSlot() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (status_.boot_primary != status_.active_slot) {
        std::swap(status_.active_slot, status_.inactive_slot);
        status_.installed_version        = staged_version_;
        status_.active_marked_good       = false;
        status_.pending_post_boot_verify = true;
        saveLocked();
    }
    return true;
}

bool MockOtaBootHAL::markCurrentSlotGood() {
    std::lock_guard<std::mutex> lk(mtx_);
    status_.active_marked_good       = true;
    status_.pending_post_boot_verify = false;
    previous_version_                = status_.installed_version;
    saveLocked();
    return true;
}

bool MockOtaBootHAL::rollbackToPreviousSlot() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::swap(status_.active_slot, status_.inactive_slot);
    status_.boot_primary             = status_.active_slot;
    status_.installed_version        = previous_version_;
    status_.active_marked_good       = true;
    status_.pending_post_boot_verify = false;
    ++status_.rollback_count;
    saveLocked();
    return true;
}

// ============================================================================
// 6. MockCloudHAL
// ============================================================================

MockCloudHAL::MockCloudHAL(std::string spool_dir)
    : spool_dir_(std::move(spool_dir)) {}

bool MockCloudHAL::connect() {
    std::error_code ec;
    std::filesystem::create_directories(spool_dir_, ec);
    connected_.store(true);
    return true;
}

void MockCloudHAL::disconnect() {
    connected_.store(false);
}

bool MockCloudHAL::publish(const std::string& topic, const std::string& payload, int /*qos*/) {
    if (!connected_.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    published_.emplace_back(topic, payload);
    if (!spool_dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(spool_dir_, ec);
        std::ofstream ofs(spool_dir_ + "/mqtt_published.jsonl", std::ios::app);
        common::JsonValue j = common::JsonValue::object();
        j["topic"]   = topic;
        j["payload"] = payload;
        ofs << j.dump(false) << "\n";
    }
    return true;
}

void MockCloudHAL::setCommandCallback(CloudCommandCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    cmd_cb_ = std::move(cb);
}

void MockCloudHAL::injectIncomingCommand(const std::string& topic, const std::string& payload) {
    CloudCommandCallback cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cb = cmd_cb_;
    }
    if (cb) {
        cb(topic, payload);
    }
}

std::vector<std::pair<std::string, std::string>> MockCloudHAL::publishedMessages() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return published_;
}

void MockCloudHAL::clearPublishedMessages() {
    std::lock_guard<std::mutex> lk(mtx_);
    published_.clear();
}

bool MockCloudHAL::downloadAssetResumable(const std::string& url,
                                          const std::string& dest_file_path,
                                          DownloadProgressCallback progress_cb) {
    if (!connected_.load()) {
        return false;
    }

    std::string src_path = url;
    const std::string kFilePrefix = "file://";
    if (src_path.rfind(kFilePrefix, 0) == 0) {
        src_path = src_path.substr(kFilePrefix.size());
    }

    if (!std::filesystem::exists(src_path)) {
        return false;
    }

    std::error_code ec;
    auto parent = std::filesystem::path(dest_file_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    const uint64_t total_size = std::filesystem::file_size(src_path, ec);
    uint64_t existing_offset  = 0;
    if (std::filesystem::exists(dest_file_path, ec)) {
        existing_offset = std::filesystem::file_size(dest_file_path, ec);
        if (existing_offset > total_size) {
            std::filesystem::remove(dest_file_path, ec);
            existing_offset = 0;
        }
    }

    if (existing_offset == total_size) {
        if (progress_cb) progress_cb(total_size, total_size);
        return true;
    }

    std::ifstream ifs(src_path, std::ios::binary);
    std::ofstream ofs(dest_file_path, std::ios::binary | std::ios::app);
    if (!ifs.is_open() || !ofs.is_open()) {
        return false;
    }

    ifs.seekg(static_cast<std::streamoff>(existing_offset), std::ios::beg);
    std::vector<char> buf(16384);
    uint64_t downloaded = existing_offset;

    while (ifs.good()) {
        if (!connected_.load()) {
            return false; // Network dropped mid-transfer; partial file preserved for HTTP 206 resume
        }
        ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = ifs.gcount();
        if (got <= 0) break;

        ofs.write(buf.data(), got);
        ofs.flush();
        downloaded += static_cast<uint64_t>(got);

        if (progress_cb && !progress_cb(downloaded, total_size)) {
            return false; // Preempted by P0 eCall; partial file preserved for resume
        }
    }

    return downloaded == total_size;
}

// ============================================================================
// 7. MockWatchdogHAL
// ============================================================================

bool MockWatchdogHAL::open(const std::string& /*device_path*/, int /*timeout_sec*/) {
    return true;
}

bool MockWatchdogHAL::kick() {
    kicks_.fetch_add(1);
    LinuxWatchdogHAL::sendSystemdNotify("WATCHDOG=1");
    return true;
}

void MockWatchdogHAL::notifySystemdReady() {
    LinuxWatchdogHAL::sendSystemdNotify("READY=1");
}

void MockWatchdogHAL::closeGracefully() {
    LinuxWatchdogHAL::sendSystemdNotify("STOPPING=1");
}

} // namespace tcu::hal

