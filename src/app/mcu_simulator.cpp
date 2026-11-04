#include "app/mcu_simulator.h"
#include "common/crc.h"
#include "common/json_mini.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tcu::app {

McuSimulator::McuSimulator(std::string socket_path, int telemetry_period_ms)
    : socket_path_(std::move(socket_path)), telemetry_period_ms_(telemetry_period_ms) {
    vehicle_.speed_kmh              = 0.0;
    vehicle_.ignition_kl15          = true;
    vehicle_.gear                   = GearPosition::PARK;
    vehicle_.doors_locked           = false;
    vehicle_.authorized_driver      = true;
    vehicle_.kl30_mv                = 12600;
    vehicle_.bub_mv                 = 4100;
    vehicle_.bub_soc                = 96;
    vehicle_.bub_soh                = 98;
    vehicle_.starter_inhibit_active = false;
}

McuSimulator::~McuSimulator() {
    stop();
}

bool McuSimulator::start() {
    if (running_.load()) return true;

    std::error_code ec;
    auto parent = std::filesystem::path(socket_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    ::unlink(socket_path_.c_str());

    listen_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(listen_fd_, 4) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    worker_ = std::thread(&McuSimulator::serverLoop, this);
    return true;
}

void McuSimulator::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (client_fd_ >= 0) {
        ::close(client_fd_);
        client_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    ::unlink(socket_path_.c_str());
}

VehicleState McuSimulator::getVehicleState() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return vehicle_;
}

PowerState McuSimulator::getPowerState() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return power_state_;
}

EcallLedMode McuSimulator::getEcallLedMode() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return led_mode_;
}

uint8_t McuSimulator::activeFlashBank() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return active_bank_;
}

uint32_t McuSimulator::activeFirmwareVersion() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return active_fw_ver_;
}

uint32_t McuSimulator::activeFirmwareCrc32() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return active_fw_crc32_;
}

void McuSimulator::sendToSoCLocked(int fd, const service::McuWireFrame& frame) {
    if (fd < 0) return;
    auto enc = frame.encode();
    ssize_t s = ::send(fd, enc.data(), enc.size(), MSG_NOSIGNAL);
    (void)s;
}

void McuSimulator::sendTelemetrySnapshotLocked(int fd) {
    if (fd < 0) return;
    service::PackedVehicleTelemetry pt{};
    pt.speed_cms         = static_cast<uint16_t>(vehicle_.speed_kmh * 100.0);
    pt.ignition_kl15     = vehicle_.ignition_kl15 ? 1U : 0U;
    pt.gear              = static_cast<uint8_t>(vehicle_.gear);
    pt.doors_locked      = vehicle_.doors_locked ? 1U : 0U;
    pt.authorized_driver = vehicle_.authorized_driver ? 1U : 0U;
    pt.kl30_mv           = vehicle_.kl30_mv;
    pt.bub_mv            = vehicle_.bub_mv;
    pt.bub_soc           = vehicle_.bub_soc;
    pt.bub_soh           = vehicle_.bub_soh;
    pt.starter_inhibit   = vehicle_.starter_inhibit_active ? 1U : 0U;

    service::McuWireFrame f;
    f.msg_id  = service::McuMsgId::MSG_VEHICLE_TELEMETRY;
    f.flags   = service::MCU_FLAG_NONE;
    f.seq_num = next_seq_.fetch_add(1);
    f.payload.resize(sizeof(pt));
    std::memcpy(f.payload.data(), &pt, sizeof(pt));
    sendToSoCLocked(fd, f);
}

void McuSimulator::setVehicleState(const VehicleState& state) {
    std::lock_guard<std::mutex> lk(mtx_);
    bool prev_cut = (vehicle_.kl30_mv < 6000);
    vehicle_ = state;
    vehicle_.using_backup_battery = (vehicle_.kl30_mv < 6000);
    sendTelemetrySnapshotLocked(client_fd_);

    if (!prev_cut && vehicle_.using_backup_battery) {
        // Automatic KL30 wire-cut BUB failover tamper IRQ!
        service::PackedTamperEvent te{};
        te.tamper_flags = TAMPER_KL30_BATTERY_CUT;
        service::McuWireFrame tf;
        tf.msg_id  = service::McuMsgId::MSG_TAMPER_EVENT;
        tf.flags   = service::MCU_FLAG_URGENT_WAKE;
        tf.seq_num = next_seq_.fetch_add(1);
        tf.payload.resize(sizeof(te));
        std::memcpy(tf.payload.data(), &te, sizeof(te));
        sendToSoCLocked(client_fd_, tf);
    }
}

void McuSimulator::injectCrash(const CrashEvent& crash) {
    std::lock_guard<std::mutex> lk(mtx_);
    power_state_   = PowerState::ACTIVE_RUN;
    wakeup_reason_ = crash.automatic_activation
                         ? WakeupReason::CRASH_AIRBAG
                         : WakeupReason::SOS_BUTTON;

    service::PackedCrashEvent pc{};
    pc.airbag_mask          = crash.airbag_mask;
    pc.dv_x_kmh             = crash.delta_v_x_kmh;
    pc.dv_y_kmh             = crash.delta_v_y_kmh;
    pc.rollover             = crash.rollover ? 1U : 0U;
    pc.automatic_activation = crash.automatic_activation ? 1U : 0U;

    service::McuWireFrame f;
    f.msg_id  = service::McuMsgId::MSG_CRASH_EVENT;
    f.flags   = service::MCU_FLAG_URGENT_WAKE;
    f.seq_num = next_seq_.fetch_add(1);
    f.payload.resize(sizeof(pc));
    std::memcpy(f.payload.data(), &pc, sizeof(pc));
    sendToSoCLocked(client_fd_, f);
}

void McuSimulator::injectTamper(uint8_t tamper_flags, float pitch_deg, float roll_deg) {
    std::lock_guard<std::mutex> lk(mtx_);
    power_state_ = PowerState::ACTIVE_RUN;
    if ((tamper_flags & TAMPER_KL30_BATTERY_CUT) != 0U) {
        vehicle_.kl30_mv              = 0;
        vehicle_.using_backup_battery = true;
        wakeup_reason_                = WakeupReason::TAMPER_KL30_CUT;
        sendTelemetrySnapshotLocked(client_fd_);
    } else {
        wakeup_reason_ = WakeupReason::IMU_TOW_TILT;
    }

    service::PackedTamperEvent te{};
    te.tamper_flags    = tamper_flags;
    te.tilt_pitch_cdeg = static_cast<int16_t>(pitch_deg * 100.0f);
    te.tilt_roll_cdeg  = static_cast<int16_t>(roll_deg * 100.0f);

    service::McuWireFrame f;
    f.msg_id  = service::McuMsgId::MSG_TAMPER_EVENT;
    f.flags   = service::MCU_FLAG_URGENT_WAKE;
    f.seq_num = next_seq_.fetch_add(1);
    f.payload.resize(sizeof(te));
    std::memcpy(f.payload.data(), &te, sizeof(te));
    sendToSoCLocked(client_fd_, f);
}

void McuSimulator::requestSleepPreparation(uint16_t grace_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    power_state_ = PowerState::PRE_SLEEP;
    service::McuWireFrame f;
    f.msg_id  = service::McuMsgId::MSG_POWER_TRANSITION;
    f.seq_num = next_seq_.fetch_add(1);
    f.payload = {static_cast<uint8_t>(PowerState::PRE_SLEEP),
                 static_cast<uint8_t>(grace_ms & 0xFFU),
                 static_cast<uint8_t>((grace_ms >> 8U) & 0xFFU)};
    sendToSoCLocked(client_fd_, f);
}

void McuSimulator::serverLoop() {
    uint64_t last_telem_ms = steadyNowMs();
    uint8_t rx_buf[2048];

    while (running_.load()) {
        pollfd pfds[2]{};
        int nfds = 1;
        pfds[0].fd     = listen_fd_;
        pfds[0].events = POLLIN;

        int cfd = -1;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cfd = client_fd_;
        }
        if (cfd >= 0) {
            pfds[1].fd     = cfd;
            pfds[1].events = POLLIN;
            nfds           = 2;
        }

        int pr = ::poll(pfds, static_cast<nfds_t>(nfds), 25);
        if (pr > 0) {
            if ((pfds[0].revents & POLLIN) != 0) {
                int accepted = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
                if (accepted >= 0) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (client_fd_ >= 0) {
                        ::close(client_fd_);
                    }
                    client_fd_ = accepted;

                    // Send initial MSG_WAKEUP_REASON + initial VehicleState snapshot
                    service::McuWireFrame wf;
                    wf.msg_id  = service::McuMsgId::MSG_WAKEUP_REASON;
                    wf.seq_num = next_seq_.fetch_add(1);
                    wf.payload = {
                        static_cast<uint8_t>(wakeup_reason_),
                        static_cast<uint8_t>(vehicle_.kl30_mv & 0xFFU),
                        static_cast<uint8_t>((vehicle_.kl30_mv >> 8U) & 0xFFU),
                        static_cast<uint8_t>(vehicle_.bub_mv & 0xFFU),
                        static_cast<uint8_t>((vehicle_.bub_mv >> 8U) & 0xFFU)};
                    sendToSoCLocked(client_fd_, wf);
                    sendTelemetrySnapshotLocked(client_fd_);
                }
            }

            if (nfds == 2 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                ssize_t n = ::recv(cfd, rx_buf, sizeof(rx_buf), 0);
                if (n <= 0) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (client_fd_ == cfd) {
                        ::close(client_fd_);
                        client_fd_ = -1;
                    }
                } else {
                    handleSoCFrame(cfd, rx_buf, static_cast<size_t>(n));
                }
            }
        }

        const uint64_t now = steadyNowMs();
        if (telemetry_period_ms_ > 0 && (now - last_telem_ms) >= static_cast<uint64_t>(telemetry_period_ms_)) {
            last_telem_ms = now;
            std::lock_guard<std::mutex> lk(mtx_);
            if (client_fd_ >= 0 && power_state_ != PowerState::SLEEP) {
                sendTelemetrySnapshotLocked(client_fd_);
            }
        }
    }
}

void McuSimulator::handleSoCFrame(int cfd, const uint8_t* raw, size_t len) {
    service::McuWireFrame f;
    bool crc_err = false;
    std::lock_guard<std::mutex> lk(mtx_);

    if (!service::McuWireFrame::decode(raw, len, f, &crc_err)) {
        if (crc_err) {
            service::McuWireFrame nack;
            nack.msg_id  = f.msg_id;
            nack.flags   = service::MCU_FLAG_IS_NACK;
            nack.seq_num = f.seq_num;
            sendToSoCLocked(cfd, nack);
        }
        return;
    }

    std::vector<uint8_t> ack_payload = {1U};

    switch (f.msg_id) {
        case service::McuMsgId::MSG_HEARTBEAT_WDG: {
            heartbeats_rx_.fetch_add(1);
            break;
        }
        case service::McuMsgId::MSG_POWER_TRANSITION: {
            if (!f.payload.empty()) {
                power_state_ = static_cast<PowerState>(f.payload[0]);
            }
            break;
        }
        case service::McuMsgId::MSG_SET_ECALL_LED: {
            if (!f.payload.empty()) {
                led_mode_ = static_cast<EcallLedMode>(f.payload[0]);
            }
            break;
        }
        case service::McuMsgId::MSG_SET_IMMOBILIZER: {
            if (!f.payload.empty()) {
                bool want_inhibit = (f.payload[0] != 0U);
                if (!want_inhibit) {
                    vehicle_.starter_inhibit_active = false;
                    ack_payload = {1U};
                } else {
                    // MCU Hardware Safety Gate (LLD Section 4.2):
                    // NEVER assert starter inhibit if vehicle speed > 0 or KL15 is ON!
                    if (vehicle_.speed_kmh == 0.0 && !vehicle_.ignition_kl15) {
                        vehicle_.starter_inhibit_active = true;
                        ack_payload = {1U};
                    } else {
                        ack_payload = {0U}; // Blocked by MCU safety gate
                    }
                }
                sendTelemetrySnapshotLocked(cfd);
            }
            break;
        }
        case service::McuMsgId::MSG_MCU_OTA_BEGIN: {
            if (f.payload.size() >= 12) {
                std::memcpy(&staged_expected_size_, f.payload.data() + 0, 4);
                std::memcpy(&staged_expected_crc32_, f.payload.data() + 4, 4);
                std::memcpy(&staged_fw_ver_, f.payload.data() + 8, 4);
                if (staged_expected_size_ > 0 && staged_expected_size_ <= 524288U) {
                    inactive_bank_buf_.assign(staged_expected_size_, 0xFFU);
                    ack_payload = {1U};
                } else {
                    ack_payload = {0U};
                }
            }
            break;
        }
        case service::McuMsgId::MSG_MCU_OTA_CHUNK: {
            if (f.payload.size() >= 6) {
                uint32_t offset = 0;
                uint16_t clen   = 0;
                std::memcpy(&offset, f.payload.data() + 0, 4);
                std::memcpy(&clen, f.payload.data() + 4, 2);
                if (f.payload.size() >= (6U + clen) &&
                    (offset + clen) <= inactive_bank_buf_.size()) {
                    std::memcpy(inactive_bank_buf_.data() + offset, f.payload.data() + 6, clen);
                    ack_payload = {1U};
                } else {
                    ack_payload = {0U};
                }
            }
            break;
        }
        case service::McuMsgId::MSG_MCU_OTA_COMMIT: {
            uint32_t calc_crc = common::computeCrc32(inactive_bank_buf_);
            if (!inactive_bank_buf_.empty() &&
                inactive_bank_buf_.size() == staged_expected_size_ &&
                calc_crc == staged_expected_crc32_) {
                active_bank_     = static_cast<uint8_t>(1U - active_bank_);
                active_fw_ver_   = staged_fw_ver_;
                active_fw_crc32_ = calc_crc;
                ack_payload      = {1U, active_bank_};
            } else {
                // Corrupt MCU image: abort swap, keep active bank intact!
                ack_payload = {0U, active_bank_};
            }
            break;
        }
        case service::McuMsgId::MSG_SIM_INJECT_CMD: {
            // JSON simulation command forwarded by tcu-mcu-ipcd for tcu-ctl multi-process mode
            std::string js(reinterpret_cast<const char*>(f.payload.data()), f.payload.size());
            if (auto j = common::JsonValue::parse(js)) {
                std::string cmd = (*j)["cmd"].asString();
                if (cmd == "set_vehicle") {
                    if (j->contains("speed_kmh")) vehicle_.speed_kmh = (*j)["speed_kmh"].asDouble();
                    if (j->contains("ign"))       vehicle_.ignition_kl15 = (*j)["ign"].asBool();
                    if (j->contains("doors_locked")) vehicle_.doors_locked = (*j)["doors_locked"].asBool();
                    if (j->contains("auth_driver"))  vehicle_.authorized_driver = (*j)["auth_driver"].asBool();
                    if (j->contains("kl30_mv"))   vehicle_.kl30_mv = static_cast<uint16_t>((*j)["kl30_mv"].asUint64());
                    if (j->contains("bub_soh"))   vehicle_.bub_soh = static_cast<uint8_t>((*j)["bub_soh"].asUint64());
                    if (j->contains("gear"))      vehicle_.gear = static_cast<GearPosition>((*j)["gear"].asUint64());
                    vehicle_.using_backup_battery = (vehicle_.kl30_mv < 6000);
                    sendTelemetrySnapshotLocked(cfd);
                } else if (cmd == "crash") {
                    service::PackedCrashEvent pc{};
                    pc.airbag_mask          = 0x01;
                    pc.dv_x_kmh             = static_cast<int16_t>((*j)["delta_v"].asInt64(-48));
                    pc.rollover             = (*j)["rollover"].asBool(false) ? 1U : 0U;
                    pc.automatic_activation = (*j)["auto"].asBool(true) ? 1U : 0U;
                    service::McuWireFrame cf;
                    cf.msg_id  = service::McuMsgId::MSG_CRASH_EVENT;
                    cf.flags   = service::MCU_FLAG_URGENT_WAKE;
                    cf.seq_num = next_seq_.fetch_add(1);
                    cf.payload.resize(sizeof(pc));
                    std::memcpy(cf.payload.data(), &pc, sizeof(pc));
                    sendToSoCLocked(cfd, cf);
                } else if (cmd == "tamper") {
                    uint8_t flags = static_cast<uint8_t>((*j)["flags"].asUint64(TAMPER_KL30_BATTERY_CUT));
                    if ((flags & TAMPER_KL30_BATTERY_CUT) != 0U) {
                        vehicle_.kl30_mv = 0;
                        vehicle_.using_backup_battery = true;
                        sendTelemetrySnapshotLocked(cfd);
                    }
                    service::PackedTamperEvent te{};
                    te.tamper_flags    = flags;
                    te.tilt_pitch_cdeg = static_cast<int16_t>((*j)["pitch_deg"].asDouble(0.0) * 100.0);
                    service::McuWireFrame tf;
                    tf.msg_id  = service::McuMsgId::MSG_TAMPER_EVENT;
                    tf.flags   = service::MCU_FLAG_URGENT_WAKE;
                    tf.seq_num = next_seq_.fetch_add(1);
                    tf.payload.resize(sizeof(te));
                    std::memcpy(tf.payload.data(), &te, sizeof(te));
                    sendToSoCLocked(cfd, tf);
                }
            }
            break;
        }
        default:
            break;
    }

    if ((f.flags & service::MCU_FLAG_ACK_REQ) != 0U) {
        service::McuWireFrame ack;
        ack.msg_id  = f.msg_id;
        ack.flags   = service::MCU_FLAG_IS_ACK;
        ack.seq_num = f.seq_num;
        ack.payload = ack_payload;
        sendToSoCLocked(cfd, ack);
    }
}

} // namespace tcu::app

