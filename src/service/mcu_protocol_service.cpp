#include "service/mcu_protocol_service.h"
#include "common/crc.h"

#include <algorithm>
#include <cstring>

namespace tcu::service {

std::vector<uint8_t> McuWireFrame::encode() const {
    const uint16_t pay_len = static_cast<uint16_t>(std::min<size_t>(payload.size(), 1024));
    std::vector<uint8_t> out(10U + pay_len, 0);

    out[0] = kMcuSof0;
    out[1] = kMcuSof1;
    out[2] = static_cast<uint8_t>(msg_id);
    out[3] = flags;
    out[4] = static_cast<uint8_t>(seq_num & 0xFFU);
    out[5] = static_cast<uint8_t>((seq_num >> 8U) & 0xFFU);
    out[6] = static_cast<uint8_t>(pay_len & 0xFFU);
    out[7] = static_cast<uint8_t>((pay_len >> 8U) & 0xFFU);

    if (pay_len > 0) {
        std::memcpy(out.data() + 8, payload.data(), pay_len);
    }

    const uint16_t crc = common::computeCrc16Ccitt(out.data(), 8U + pay_len);
    out[8U + pay_len] = static_cast<uint8_t>(crc & 0xFFU);
    out[9U + pay_len] = static_cast<uint8_t>((crc >> 8U) & 0xFFU);
    return out;
}

bool McuWireFrame::decode(const uint8_t* raw, size_t len, McuWireFrame& out, bool* crc_error) {
    if (crc_error) *crc_error = false;
    if (!raw || len < 10U || raw[0] != kMcuSof0 || raw[1] != kMcuSof1) {
        return false;
    }

    const uint16_t pay_len = static_cast<uint16_t>(raw[6]) |
                             static_cast<uint16_t>(static_cast<uint16_t>(raw[7]) << 8U);
    if (pay_len > 1024U || len < (10U + pay_len)) {
        return false;
    }

    const uint16_t expected_crc = common::computeCrc16Ccitt(raw, 8U + pay_len);
    const uint16_t actual_crc   = static_cast<uint16_t>(raw[8U + pay_len]) |
                                  static_cast<uint16_t>(static_cast<uint16_t>(raw[9U + pay_len]) << 8U);

    out.msg_id  = static_cast<McuMsgId>(raw[2]);
    out.flags   = raw[3];
    out.seq_num = static_cast<uint16_t>(raw[4]) |
                  static_cast<uint16_t>(static_cast<uint16_t>(raw[5]) << 8U);

    if (expected_crc != actual_crc) {
        if (crc_error) *crc_error = true;
        return false;
    }

    out.payload.assign(raw + 8, raw + 8 + pay_len);
    return true;
}

McuProtocolService::McuProtocolService(hal::IMcuIpcHAL& hal, int ack_timeout_ms, int max_retries)
    : hal_(hal), ack_timeout_ms_(ack_timeout_ms), max_retries_(max_retries) {}

McuProtocolService::~McuProtocolService() {
    stop();
}

bool McuProtocolService::start() {
    if (running_.exchange(true)) {
        return true;
    }
    rx_thread_ = std::thread(&McuProtocolService::rxLoop, this);
    return true;
}

void McuProtocolService::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        for (auto& [seq, p] : pending_acks_) {
            (void)seq;
            std::lock_guard<std::mutex> plk(p->mtx);
            p->resolved = true;
            p->is_ack   = false;
            p->cv.notify_all();
        }
        pending_acks_.clear();
    }
    if (rx_thread_.joinable()) {
        rx_thread_.join();
    }
}

void McuProtocolService::setFrameCallback(FrameCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mtx_);
    frame_cb_ = std::move(cb);
}

bool McuProtocolService::sendUnreliable(McuMsgId msg_id,
                                        const std::vector<uint8_t>& payload,
                                        uint8_t extra_flags) {
    McuWireFrame f;
    f.msg_id  = msg_id;
    f.flags   = extra_flags;
    f.seq_num = next_seq_.fetch_add(1);
    f.payload = payload;
    auto encoded = f.encode();
    return hal_.sendFrame(encoded.data(), encoded.size());
}

bool McuProtocolService::sendReliable(McuMsgId msg_id,
                                      const std::vector<uint8_t>& payload,
                                      std::vector<uint8_t>* ack_payload_out,
                                      int custom_timeout_ms) {
    const uint16_t seq = next_seq_.fetch_add(1);
    McuWireFrame f;
    f.msg_id  = msg_id;
    f.flags   = MCU_FLAG_ACK_REQ;
    f.seq_num = seq;
    f.payload = payload;
    const auto encoded = f.encode();

    const int wait_ms = (custom_timeout_ms > 0) ? custom_timeout_ms : ack_timeout_ms_;

    for (int attempt = 0; attempt <= max_retries_; ++attempt) {
        auto pending = std::make_shared<PendingAck>();
        {
            std::lock_guard<std::mutex> lk(pending_mtx_);
            pending_acks_[seq] = pending;
        }

        if (!hal_.sendFrame(encoded.data(), encoded.size())) {
            std::lock_guard<std::mutex> lk(pending_mtx_);
            pending_acks_.erase(seq);
            consecutive_failures_.fetch_add(1);
            return false;
        }

        std::unique_lock<std::mutex> plk(pending->mtx);
        bool got_resp = pending->cv.wait_for(
            plk, std::chrono::milliseconds(wait_ms), [&] { return pending->resolved; });

        {
            std::lock_guard<std::mutex> lk(pending_mtx_);
            pending_acks_.erase(seq);
        }

        if (got_resp && pending->is_ack) {
            consecutive_failures_.store(0);
            if (ack_payload_out) {
                *ack_payload_out = std::move(pending->payload);
            }
            return true;
        }
        timeouts_.fetch_add(1);
    }

    consecutive_failures_.fetch_add(1);
    return false;
}

bool McuProtocolService::flashMcuFirmware(const std::vector<uint8_t>& fw_image,
                                          uint32_t fw_version_code) {
    if (fw_image.empty()) {
        return false;
    }

    const uint32_t total_size = static_cast<uint32_t>(fw_image.size());
    const uint32_t crc32      = common::computeCrc32(fw_image);

    // 1. MSG_MCU_OTA_BEGIN (0x40): [fw_size(4B), expected_crc32(4B), fw_version(4B)]
    std::vector<uint8_t> begin_pay(12, 0);
    std::memcpy(begin_pay.data() + 0, &total_size, 4);
    std::memcpy(begin_pay.data() + 4, &crc32, 4);
    std::memcpy(begin_pay.data() + 8, &fw_version_code, 4);

    if (!sendReliable(McuMsgId::MSG_MCU_OTA_BEGIN, begin_pay, nullptr, 500)) {
        return false;
    }

    // 2. MSG_MCU_OTA_CHUNK (0x41): [offset(4B), chunk_len(2B), data(<=512B)]
    constexpr size_t kChunkSize = 512;
    for (size_t offset = 0; offset < fw_image.size(); offset += kChunkSize) {
        const uint16_t clen = static_cast<uint16_t>(
            std::min<size_t>(kChunkSize, fw_image.size() - offset));
        const uint32_t u32_off = static_cast<uint32_t>(offset);

        std::vector<uint8_t> chunk_pay(6U + clen);
        std::memcpy(chunk_pay.data() + 0, &u32_off, 4);
        std::memcpy(chunk_pay.data() + 4, &clen, 2);
        std::memcpy(chunk_pay.data() + 6, fw_image.data() + offset, clen);

        if (!sendReliable(McuMsgId::MSG_MCU_OTA_CHUNK, chunk_pay, nullptr, 500)) {
            return false;
        }
    }

    // 3. MSG_MCU_OTA_COMMIT (0x42): [swap_bank_and_reset = 1]
    std::vector<uint8_t> commit_pay = {1U};
    std::vector<uint8_t> ack_pay;
    if (!sendReliable(McuMsgId::MSG_MCU_OTA_COMMIT, commit_pay, &ack_pay, 1000)) {
        return false;
    }
    return !ack_pay.empty() && ack_pay[0] == 1U;
}

void McuProtocolService::rxLoop() {
    std::vector<uint8_t> raw;
    while (running_.load()) {
        int r = hal_.receiveFrame(raw, 40);
        if (r <= 0) {
            continue;
        }

        McuWireFrame decoded;
        bool crc_err = false;
        if (!McuWireFrame::decode(raw.data(), raw.size(), decoded, &crc_err)) {
            if (crc_err) {
                crc_errors_.fetch_add(1);
                // Immediately send NACK so MCU can retransmit
                McuWireFrame nack;
                nack.msg_id  = decoded.msg_id;
                nack.flags   = MCU_FLAG_IS_NACK;
                nack.seq_num = decoded.seq_num;
                auto enc     = nack.encode();
                hal_.sendFrame(enc.data(), enc.size());
            }
            continue;
        }

        consecutive_failures_.store(0);

        if ((decoded.flags & (MCU_FLAG_IS_ACK | MCU_FLAG_IS_NACK)) != 0U) {
            std::shared_ptr<PendingAck> pending;
            {
                std::lock_guard<std::mutex> lk(pending_mtx_);
                auto it = pending_acks_.find(decoded.seq_num);
                if (it != pending_acks_.end()) {
                    pending = it->second;
                }
            }
            if (pending) {
                std::lock_guard<std::mutex> plk(pending->mtx);
                pending->is_ack   = (decoded.flags & MCU_FLAG_IS_ACK) != 0U;
                pending->payload  = decoded.payload;
                pending->resolved = true;
                pending->cv.notify_all();
            }
            continue;
        }

        if ((decoded.flags & MCU_FLAG_ACK_REQ) != 0U) {
            McuWireFrame ack;
            ack.msg_id  = decoded.msg_id;
            ack.flags   = MCU_FLAG_IS_ACK;
            ack.seq_num = decoded.seq_num;
            auto enc    = ack.encode();
            hal_.sendFrame(enc.data(), enc.size());
        }

        FrameCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx_);
            cb = frame_cb_;
        }
        if (cb) {
            cb(decoded);
        }
    }
}

} // namespace tcu::service

