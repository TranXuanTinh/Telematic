#include "app/mcu_simulator.h"
#include "common/crc.h"
#include "hal/mock/mock_hals.h"
#include "service/mcu_protocol_service.h"
#include "test_cluster_harness.h"

#include <filesystem>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace {

/// Wrapper HAL that corrupts the first outbound frame of a targeted McuMsgId to verify CRC NACK + Retry.
class FaultInjectingMcuHal : public tcu::hal::IMcuIpcHAL {
public:
    explicit FaultInjectingMcuHal(tcu::hal::IMcuIpcHAL& inner) : inner_(inner) {}

    bool open(const tcu::hal::McuLinkOpenParams& p) override { return inner_.open(p); }
    void close() override { inner_.close(); }
    bool isOpen() const override { return inner_.isOpen(); }

    bool sendFrame(const uint8_t* data, size_t len) override {
        if (corrupt_next_.exchange(false) && len > 6) {
            std::vector<uint8_t> bad(data, data + len);
            bad[len - 1] ^= 0xFFU; // Corrupt CRC-16 byte -> MCU must reply NACK -> service retries!
            corrupted_count_.fetch_add(1);
            return inner_.sendFrame(bad.data(), bad.size());
        }
        return inner_.sendFrame(data, len);
    }

    int receiveFrame(std::vector<uint8_t>& out, int timeout_ms) override {
        return inner_.receiveFrame(out, timeout_ms);
    }

    void armSingleCrcCorruption() { corrupt_next_.store(true); }
    uint32_t corruptedCount() const { return corrupted_count_.load(); }

private:
    tcu::hal::IMcuIpcHAL& inner_;
    std::atomic<bool>     corrupt_next_{false};
    std::atomic<uint32_t> corrupted_count_{0};
};

} // namespace

int main() {
    using namespace tcu;

    std::string dir = "/tmp/tcu_mcu_t_" + std::to_string(::getpid());
    std::filesystem::create_directories(dir);
    std::string sock = dir + "/mcu.sock";

    app::McuSimulator sim(sock, 50);
    TCU_REQUIRE(sim.start());

    hal::MockMcuIpcHAL raw_hal;
    hal::McuLinkOpenParams open_params;
    open_params.device_or_socket = sock;
    open_params.use_mock_socket  = true;
    TCU_REQUIRE(raw_hal.open(open_params));

    FaultInjectingMcuHal faulty_hal(raw_hal);
    service::McuProtocolService proto(faulty_hal, 60, 3);
    TCU_REQUIRE(proto.start());

    // 1. Reliable Heartbeat with injected CRC-16 bit-flip -> MCU sends NACK -> automatic retry succeeds!
    faulty_hal.armSingleCrcCorruption();
    std::vector<uint8_t> hb_payload = {static_cast<uint8_t>(PowerState::ACTIVE_RUN), 0x00, 1, 2, 3, 4};
    TCU_REQUIRE(proto.sendReliable(service::McuMsgId::MSG_HEARTBEAT_WDG, hb_payload));
    TCU_REQUIRE(faulty_hal.corruptedCount() == 1U);
    TCU_REQUIRE(sim.heartbeatCount() >= 1U);

    // 2. Dual-Bank Companion MCU Firmware DFU (1.5 KB chunked image -> Bank A (0) swaps to Bank B (1))
    TCU_REQUIRE(sim.activeFlashBank() == 0U);
    std::vector<uint8_t> fw_img(1500);
    for (size_t i = 0; i < fw_img.size(); ++i) {
        fw_img[i] = static_cast<uint8_t>((i * 37U + 11U) & 0xFFU);
    }
    TCU_REQUIRE(proto.flashMcuFirmware(fw_img, 0x00010800U));
    TCU_REQUIRE(sim.activeFlashBank() == 1U);
    TCU_REQUIRE(sim.activeFirmwareVersion() == 0x00010800U);
    TCU_REQUIRE(sim.activeFirmwareCrc32() == common::computeCrc32(fw_img));

    // 3. Corrupt MCU Firmware Commit Rejection (wrong expected CRC32 must keep Bank B active!)
    uint32_t bad_size = 64;
    uint32_t bad_crc  = 0xDEADBEEFU;
    uint32_t bad_ver  = 0x00090909U;
    std::vector<uint8_t> begin_pay(12);
    std::memcpy(begin_pay.data() + 0, &bad_size, 4);
    std::memcpy(begin_pay.data() + 4, &bad_crc, 4);
    std::memcpy(begin_pay.data() + 8, &bad_ver, 4);
    TCU_REQUIRE(proto.sendReliable(service::McuMsgId::MSG_MCU_OTA_BEGIN, begin_pay));

    std::vector<uint8_t> chunk_pay(6 + 64, 0xAAU);
    uint32_t off = 0;
    uint16_t clen = 64;
    std::memcpy(chunk_pay.data() + 0, &off, 4);
    std::memcpy(chunk_pay.data() + 4, &clen, 2);
    TCU_REQUIRE(proto.sendReliable(service::McuMsgId::MSG_MCU_OTA_CHUNK, chunk_pay));

    std::vector<uint8_t> commit_ack;
    TCU_REQUIRE(proto.sendReliable(service::McuMsgId::MSG_MCU_OTA_COMMIT, {}, &commit_ack));
    TCU_REQUIRE(!commit_ack.empty() && commit_ack[0] == 0U); // Commit rejected!
    TCU_REQUIRE(sim.activeFlashBank() == 1U);
    TCU_REQUIRE(sim.activeFirmwareVersion() == 0x00010800U);

    proto.stop();
    raw_hal.close();
    sim.stop();
    std::filesystem::remove_all(dir);

    std::cout << "[PASS] test_mcu_protocol_and_sim\n";
    return 0;
}
