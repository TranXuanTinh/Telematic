/**
 * @file interview_coding_drills.cpp
 * @brief Self-contained C++17 Live-Coding Practice Suite for Senior Telematics Software Engineer Interviews.
 *
 * Covers the 5 highest-frequency embedded telematics coding exercises:
 *   Drill A: SAE J1939 29-bit CAN ID -> Priority, PGN (PDU1 vs PDU2), Destination & Source Address
 *   Drill B: General CAN Signal Decoder (Intel Little-Endian & Motorola Big-Endian, Two's Complement, SNA check)
 *   Drill C: Bounded Thread-Safe Priority Store-and-Forward Queue with Acknowledgment Boundary
 *   Drill D: Overflow-Safe Capped Exponential Backoff with Full Jitter (Testable via Injected RNG)
 *   Drill E: Stateful Framed Binary Byte-Stream Parser (0x55 0xAA + Length + CRC-16-CCITT + Noise Resync)
 *
 * Compile & Run:
 *   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread \
 *       interview_prep/coding_practice/interview_coding_drills.cpp -o /tmp/coding_drills && /tmp/coding_drills
 */

#include <algorithm>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace drills {

// ============================================================================
// DRILL A: SAE J1939 29-bit Identifier & PGN Extractor (PDU1 vs PDU2)
// ============================================================================
struct J1939Header {
    uint8_t  priority{0};
    uint32_t pgn{0};
    uint8_t  pf{0};
    uint8_t  ps{0};
    uint8_t  source_addr{0};
    uint8_t  dest_addr{0xFF}; // 0xFF = Global/Broadcast for PDU2 or broadcast PDU1
};

inline J1939Header decodeJ1939CanId(uint32_t raw_can_id) noexcept {
    // Mask off SocketCAN flags (bit 31 CAN_EFF_FLAG, bit 30 RTR, bit 29 ERR)
    const uint32_t id29 = raw_can_id & 0x1FFFFFFFU;

    J1939Header h;
    h.priority    = static_cast<uint8_t>((id29 >> 26U) & 0x07U);
    h.pf          = static_cast<uint8_t>((id29 >> 16U) & 0xFFU);
    h.ps          = static_cast<uint8_t>((id29 >> 8U) & 0xFFU);
    h.source_addr = static_cast<uint8_t>(id29 & 0xFFU);

    // 18-bit PGN candidate: [Reserved/EDP (bit 25) | Data Page (bit 24) | PF | PS]
    h.pgn = (id29 >> 8U) & 0x3FFFFU;
    if (h.pf < 240U) {
        // PDU1 (Peer-to-Peer or Global 0xFF): PS is Destination Address, NOT part of PGN!
        h.dest_addr = h.ps;
        h.pgn &= 0x3FF00U;
    } else {
        // PDU2 (Broadcast): PS is Group Extension and IS part of PGN!
        h.dest_addr = 0xFFU;
    }
    return h;
}

// ============================================================================
// DRILL B: Signed & Unsigned CAN Signal Decoder (Intel LE + SNA Validation)
// ============================================================================
struct DecodedSignal {
    double physical_value{0.0};
    bool   is_valid{true}; // false if J1939 Error (0xFE..) or Not Available / SNA (0xFF..)
};

inline int64_t signExtendTwosComplement(uint64_t raw, uint8_t bit_len) noexcept {
    if (bit_len == 0U || bit_len >= 64U) {
        return static_cast<int64_t>(raw);
    }
    const uint64_t mask     = (1ULL << bit_len) - 1ULL;
    const uint64_t sign_bit = 1ULL << (bit_len - 1U);
    raw &= mask;
    if ((raw & sign_bit) != 0ULL) {
        return static_cast<int64_t>(raw) - static_cast<int64_t>(1ULL << bit_len);
    }
    return static_cast<int64_t>(raw);
}

inline std::optional<DecodedSignal> decodeIntelCanSignal(const uint8_t* frame_data,
                                                         size_t frame_len,
                                                         uint16_t start_bit,
                                                         uint8_t bit_len,
                                                         bool is_signed,
                                                         double factor,
                                                         double offset,
                                                         bool check_j1939_sna = false) noexcept {
    if (!frame_data || bit_len == 0U || bit_len > 64U) {
        return std::nullopt;
    }
    if (static_cast<size_t>(start_bit) + bit_len > frame_len * 8U) {
        return std::nullopt; // Out-of-bounds frame access prevented!
    }

    uint64_t raw = 0ULL;
    for (uint8_t i = 0; i < bit_len; ++i) {
        const size_t bit_idx  = static_cast<size_t>(start_bit) + i;
        const size_t byte_idx = bit_idx / 8U;
        const uint8_t bit_pos = static_cast<uint8_t>(bit_idx % 8U);
        if ((frame_data[byte_idx] & (1U << bit_pos)) != 0U) {
            raw |= (1ULL << i);
        }
    }

    // Optional J1939 SNA / Error Indicator check for unsigned 8-bit & 16-bit parameters
    if (check_j1939_sna && !is_signed) {
        if ((bit_len == 8U && raw >= 0xFEULL) || (bit_len == 16U && raw >= 0xFE00ULL)) {
            return DecodedSignal{0.0, false};
        }
    }

    double phys = 0.0;
    if (is_signed) {
        phys = static_cast<double>(signExtendTwosComplement(raw, bit_len)) * factor + offset;
    } else {
        phys = static_cast<double>(raw) * factor + offset;
    }
    return DecodedSignal{phys, true};
}

// ============================================================================
// DRILL C: Bounded Priority Telemetry Queue with Acknowledgment Boundary
// ============================================================================
enum class EventPriority : uint8_t {
    P0_EMERGENCY = 0, // Highest priority (eCall, Crash) - never evicted by lower priority
    P1_SECURITY  = 1, // SVT Tamper, Critical Confirmed DTC
    P2_NORMAL    = 2  // Routine periodic telemetry (evicted first when storage is full)
};

struct TelemetryRecord {
    uint64_t      event_id{0};
    EventPriority priority{EventPriority::P2_NORMAL};
    uint64_t      created_mono_ms{0};
    uint64_t      ttl_ms{0}; // 0 = no expiry
    std::string   payload;
};

class BoundedTelemetryQueue {
public:
    explicit BoundedTelemetryQueue(size_t capacity) : capacity_(capacity) {
        assert(capacity_ > 0U);
    }

    bool enqueue(TelemetryRecord rec, uint64_t now_mono_ms) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (shutdown_) return false;

        purgeExpiredLocked(now_mono_ms);

        if (queue_.size() >= capacity_) {
            // Find the lowest-priority (numerically largest), oldest record to evict
            auto victim = queue_.end();
            for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                if (it->priority > rec.priority) {
                    if (victim == queue_.end() || it->priority > victim->priority) {
                        victim = it;
                    }
                }
            }
            if (victim == queue_.end()) {
                // Queue is full of equal-or-higher priority events!
                ++dropped_count_;
                return false;
            }
            queue_.erase(victim);
            ++evicted_count_;
        }

        // Insert ordered by priority (P0 first, FIFO within same priority)
        auto pos = std::find_if(queue_.begin(), queue_.end(), [&](const TelemetryRecord& existing) {
            return existing.priority > rec.priority;
        });
        queue_.insert(pos, std::move(rec));
        cv_.notify_one();
        return true;
    }

    // Peek front without deleting until broker PUBACK / HTTP 200 is confirmed!
    std::optional<TelemetryRecord> peekNextForUpload(uint64_t now_mono_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        purgeExpiredLocked(now_mono_ms);
        if (queue_.empty()) return std::nullopt;
        return queue_.front();
    }

    bool acknowledgeDelivered(uint64_t event_id) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = std::find_if(queue_.begin(), queue_.end(), [&](const TelemetryRecord& r) {
            return r.event_id == event_id;
        });
        if (it == queue_.end()) return false;
        queue_.erase(it);
        return true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lk(mtx_);
        shutdown_ = true;
        cv_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return queue_.size();
    }
    size_t evictedCount() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return evicted_count_;
    }
    size_t droppedCount() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return dropped_count_;
    }

private:
    void purgeExpiredLocked(uint64_t now_mono_ms) {
        queue_.erase(
            std::remove_if(queue_.begin(), queue_.end(), [&](const TelemetryRecord& r) {
                return r.ttl_ms > 0U && (now_mono_ms > r.created_mono_ms) &&
                       ((now_mono_ms - r.created_mono_ms) > r.ttl_ms);
            }),
            queue_.end());
    }

    size_t                        capacity_;
    mutable std::mutex            mtx_;
    std::condition_variable       cv_;
    std::deque<TelemetryRecord>   queue_;
    bool                          shutdown_{false};
    size_t                        evicted_count_{0};
    size_t                        dropped_count_{0};
};

// ============================================================================
// DRILL D: Overflow-Safe Capped Exponential Backoff with Full Jitter
// ============================================================================
class ExponentialBackoff {
public:
    // UniformGenerator(max_inclusive) -> [0 .. max_inclusive]
    using UniformRng = std::function<uint64_t(uint64_t)>;

    ExponentialBackoff(uint64_t base_ms, uint64_t cap_ms, UniformRng rng)
        : base_ms_(base_ms), cap_ms_(std::max(base_ms, cap_ms)), rng_(std::move(rng)) {}

    uint64_t nextDelayMs() {
        // Clamp shift at 30 to guarantee zero shift-UB or uint64 multiplication overflow
        const uint32_t shift = std::min(attempt_, 30U);
        ++attempt_;

        const uint64_t factor = 1ULL << shift;
        uint64_t ceiling_ms   = cap_ms_;
        if (base_ms_ <= cap_ms_ / factor) {
            ceiling_ms = base_ms_ * factor;
        }
        return rng_ ? rng_(ceiling_ms) : ceiling_ms;
    }

    void reset() noexcept { attempt_ = 0U; }
    uint32_t attempts() const noexcept { return attempt_; }

private:
    uint64_t   base_ms_;
    uint64_t   cap_ms_;
    UniformRng rng_;
    uint32_t   attempt_{0};
};

// ============================================================================
// DRILL E: Stateful Framed Byte-Stream Parser (0x55 0xAA + CRC-16-CCITT)
// ============================================================================
inline uint16_t crc16Ccitt(const uint8_t* data, size_t len) noexcept {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8U;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U)
                                  : static_cast<uint16_t>(crc << 1U);
        }
    }
    return crc;
}

/**
 * Wire layout:
 *   [0] 0x55 | [1] 0xAA | [2] msg_id | [3..4] payload_len (LE) | [5..5+N-1] payload | [CRC16 LE (2B)]
 */
class FramedStreamParser {
public:
    static constexpr size_t kHeaderSize     = 5U;
    static constexpr size_t kCrcSize        = 2U;
    static constexpr size_t kMaxPayloadSize = 512U;
    static constexpr size_t kMaxBufferCap   = 2048U;

    struct ParsedFrame {
        uint8_t              msg_id{0};
        std::vector<uint8_t> payload;
    };

    using FrameCallback = std::function<void(const ParsedFrame&)>;

    explicit FramedStreamParser(FrameCallback cb) : cb_(std::move(cb)) {
        rx_buf_.reserve(kMaxBufferCap);
    }

    static std::vector<uint8_t> buildFrame(uint8_t msg_id, const std::vector<uint8_t>& payload) {
        const uint16_t plen = static_cast<uint16_t>(payload.size());
        std::vector<uint8_t> out;
        out.reserve(kHeaderSize + plen + kCrcSize);
        out.push_back(0x55U);
        out.push_back(0xAAU);
        out.push_back(msg_id);
        out.push_back(static_cast<uint8_t>(plen & 0xFFU));
        out.push_back(static_cast<uint8_t>((plen >> 8U) & 0xFFU));
        out.insert(out.end(), payload.begin(), payload.end());

        // CRC-16 over [msg_id .. end of payload]
        const uint16_t crc = crc16Ccitt(out.data() + 2U, out.size() - 2U);
        out.push_back(static_cast<uint8_t>(crc & 0xFFU));
        out.push_back(static_cast<uint8_t>((crc >> 8U) & 0xFFU));
        return out;
    }

    void feedBytes(const uint8_t* chunk, size_t len) {
        if (!chunk || len == 0U) return;

        size_t offset = 0U;
        while (offset < len) {
            const size_t space = (rx_buf_.size() < kMaxBufferCap) ? (kMaxBufferCap - rx_buf_.size()) : 1U;
            const size_t take  = std::min(len - offset, space);
            if (rx_buf_.size() + take > kMaxBufferCap) {
                // Prevent unbounded memory growth under continuous garbage flood
                rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + static_cast<std::ptrdiff_t>(take));
            }
            rx_buf_.insert(rx_buf_.end(), chunk + offset, chunk + offset + take);
            offset += take;
            drainValidFrames();
        }
    }

    uint32_t crcFailureCount() const noexcept { return crc_failures_; }

private:
    void drainValidFrames() {
        while (rx_buf_.size() >= 2U) {
            // 1. Hunt for Start-of-Frame 0x55 0xAA
            size_t sof_pos = 0U;
            bool found_sof = false;
            for (; sof_pos + 1U < rx_buf_.size(); ++sof_pos) {
                if (rx_buf_[sof_pos] == 0x55U && rx_buf_[sof_pos + 1U] == 0xAAU) {
                    found_sof = true;
                    break;
                }
            }
            if (!found_sof) {
                // Keep trailing 0x55 in case 0xAA arrives in the next UART/SPI read!
                if (rx_buf_.back() == 0x55U) {
                    rx_buf_.erase(rx_buf_.begin(), rx_buf_.end() - 1);
                } else {
                    rx_buf_.clear();
                }
                return;
            }
            if (sof_pos > 0U) {
                rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + static_cast<std::ptrdiff_t>(sof_pos));
            }

            // 2. Wait until full 5-byte header is available
            if (rx_buf_.size() < kHeaderSize) return;

            const uint16_t plen = static_cast<uint16_t>(rx_buf_[3]) |
                                  (static_cast<uint16_t>(rx_buf_[4]) << 8U);

            // 3. Reject bogus payload_len immediately and resynchronize past false 0x55 0xAA!
            if (plen > kMaxPayloadSize) {
                rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + 2);
                continue;
            }

            const size_t total_frame_bytes = kHeaderSize + static_cast<size_t>(plen) + kCrcSize;
            if (rx_buf_.size() < total_frame_bytes) {
                return; // Wait for remaining fragmented bytes
            }

            // 4. Verify CRC-16-CCITT before dispatching
            const uint16_t rx_crc =
                static_cast<uint16_t>(rx_buf_[kHeaderSize + plen]) |
                (static_cast<uint16_t>(rx_buf_[kHeaderSize + plen + 1U]) << 8U);
            const uint16_t calc_crc = crc16Ccitt(rx_buf_.data() + 2U, 3U + plen);

            if (rx_crc != calc_crc) {
                ++crc_failures_;
                // Advance past false/corrupted SOF so an embedded valid frame isn't lost
                rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + 2);
                continue;
            }

            ParsedFrame pf;
            pf.msg_id = rx_buf_[2];
            pf.payload.assign(rx_buf_.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                              rx_buf_.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + plen));
            rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + static_cast<std::ptrdiff_t>(total_frame_bytes));

            if (cb_) cb_(pf);
        }
    }

    FrameCallback        cb_;
    std::vector<uint8_t> rx_buf_;
    uint32_t             crc_failures_{0};
};

} // namespace drills

int main() {
    using namespace drills;

    // --- Verify Drill A: J1939 PGN Extraction ---
    {
        // PDU2 (EEC1: 0x18F00401, with SocketCAN EFF bit 0x80000000 set)
        auto h1 = decodeJ1939CanId(0x80000000U | 0x18F00401U);
        assert(h1.priority == 6U);
        assert(h1.pgn == 0x00F004U);
        assert(h1.source_addr == 0x01U);

        // PDU1 (Request PGN: 0x18EAFF01 -> PF = 0xEA = 234 < 240, DA = 0xFF -> PGN = 0x00EA00)
        auto h2 = decodeJ1939CanId(0x18EAFF01U);
        assert(h2.pgn == 0x00EA00U);
        assert(h2.dest_addr == 0xFFU);
        assert(h2.source_addr == 0x01U);
        std::cout << "[PASS] Drill A: J1939 PGN Extractor (PDU1 & PDU2)\n";
    }

    // --- Verify Drill B: Signed Little-Endian CAN Signal & J1939 SNA ---
    {
        // Bytes FE FF = -2 in 16-bit two's complement; factor=0.1, offset=-40 -> -40.2
        const uint8_t raw_neg[2] = {0xFEU, 0xFFU};
        auto s1 = decodeIntelCanSignal(raw_neg, 2, 0, 16, true, 0.1, -40.0);
        assert(s1.has_value() && s1->is_valid);
        assert(std::fabs(s1->physical_value - (-40.2)) < 1e-6);

        // J1939 unsigned 8-bit SNA (0xFF) -> marked invalid rather than scaled!
        const uint8_t raw_sna[1] = {0xFFU};
        auto s2 = decodeIntelCanSignal(raw_sna, 1, 0, 8, false, 0.4, 0.0, true);
        assert(s2.has_value() && !s2->is_valid);
        std::cout << "[PASS] Drill B: Signed CAN Signal Decoder & SNA Check\n";
    }

    // --- Verify Drill C: Bounded Priority Store-and-Forward Queue ---
    {
        BoundedTelemetryQueue q(2);
        assert(q.enqueue({101, EventPriority::P2_NORMAL, 1000, 5000, "norm-1"}, 1000));
        assert(q.enqueue({102, EventPriority::P2_NORMAL, 1010, 5000, "norm-2"}, 1010));
        // Queue full -> P0 Emergency must evict oldest P2 Normal AND jump to front!
        assert(q.enqueue({999, EventPriority::P0_EMERGENCY, 1020, 0, "ecall-msd"}, 1020));
        assert(q.evictedCount() == 1U);

        auto front = q.peekNextForUpload(1030);
        assert(front.has_value() && front->event_id == 999U);
        assert(q.size() == 2U); // Still persisted until ACK!
        assert(q.acknowledgeDelivered(999U));
        assert(q.size() == 1U);
        std::cout << "[PASS] Drill C: Bounded Priority Queue with ACK Boundary\n";
    }

    // --- Verify Drill D: Capped Exponential Backoff with Deterministic RNG ---
    {
        ExponentialBackoff bo(1000, 10000, [](uint64_t ceiling) { return ceiling / 2U; });
        assert(bo.nextDelayMs() == 500U);  // ceiling 1000 -> 500
        assert(bo.nextDelayMs() == 1000U); // ceiling 2000 -> 1000
        assert(bo.nextDelayMs() == 2000U); // ceiling 4000 -> 2000
        assert(bo.nextDelayMs() == 4000U); // ceiling 8000 -> 4000
        assert(bo.nextDelayMs() == 5000U); // capped at 10000 -> 5000
        bo.reset();
        assert(bo.nextDelayMs() == 500U);
        std::cout << "[PASS] Drill D: Overflow-Safe Capped Exponential Backoff\n";
    }

    // --- Verify Drill E: Stateful Framed Byte-Stream Parser ---
    {
        std::vector<FramedStreamParser::ParsedFrame> rx_frames;
        FramedStreamParser parser([&](const auto& f) { rx_frames.push_back(f); });

        auto f1 = FramedStreamParser::buildFrame(0x11, {0xDE, 0xAD});
        auto f2 = FramedStreamParser::buildFrame(0x22, {0xBE, 0xEF, 0x01});

        // Build adversarial stream: leading garbage + false 0x55 0xAA with bogus length + f1 + corrupted frame + f2
        std::vector<uint8_t> stream = {0x99, 0x00, 0x55, 0xAA, 0x01, 0xFF, 0x7F};
        stream.insert(stream.end(), f1.begin(), f1.end());
        auto bad = f1;
        bad.back() ^= 0xFFU; // Flip CRC bit
        stream.insert(stream.end(), bad.begin(), bad.end());
        stream.insert(stream.end(), f2.begin(), f2.end());

        // Feed 1 byte at a time to verify fragmentation & resync!
        for (uint8_t b : stream) {
            parser.feedBytes(&b, 1U);
        }

        assert(rx_frames.size() == 2U);
        assert(rx_frames[0].msg_id == 0x11U && rx_frames[0].payload.size() == 2U);
        assert(rx_frames[1].msg_id == 0x22U && rx_frames[1].payload.size() == 3U);
        assert(parser.crcFailureCount() == 1U);
        std::cout << "[PASS] Drill E: Stateful Framed Byte-Stream Parser (1-byte fragmentation + noise resync)\n";
    }

    std::cout << "\nAll 5 Telematics Live-Coding Drills PASSED!\n";
    return 0;
}
