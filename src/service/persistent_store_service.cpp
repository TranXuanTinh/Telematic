#include "service/persistent_store_service.h"
#include "common/json_mini.h"

namespace tcu::service {

PersistentStoreService::PersistentStoreService(hal::IStorageHAL& storage,
                                               hal::ICryptoHAL& crypto,
                                               const std::string& key_hex)
    : storage_(storage), crypto_(crypto) {
    aes_key_ = common::hexToBytes(key_hex);
    if (aes_key_.size() < 32) {
        aes_key_.resize(32, 0xA5U);
    }
}

bool PersistentStoreService::storeEncryptedBreadcrumb(int priority,
                                                      const std::string& topic,
                                                      const std::string& json_payload) {
    auto ct = crypto_.encryptAes256Gcm(json_payload, aes_key_);
    if (ct.empty()) {
        return false;
    }
    return storage_.enqueueTelemetry(priority, topic, ct);
}

std::vector<PersistentStoreService::DecryptedRow>
PersistentStoreService::popOrPeekDecryptedBatch(size_t limit) {
    std::vector<DecryptedRow> out;
    auto rows = storage_.fetchPendingTelemetry(limit);
    for (const auto& r : rows) {
        std::string plain = crypto_.decryptAes256Gcm(r.encrypted_payload, aes_key_);
        if (!plain.empty()) {
            out.push_back(DecryptedRow{r.id, r.priority, r.topic, std::move(plain)});
        } else {
            // Corrupted row: discard so queue does not stall
            storage_.removeTelemetryById(r.id);
        }
    }
    return out;
}

bool PersistentStoreService::acknowledgeDelivered(int64_t id) {
    return storage_.removeTelemetryById(id);
}

size_t PersistentStoreService::pendingCount() const {
    return storage_.getPendingTelemetryCount();
}

bool PersistentStoreService::recordFaultDtc(const std::string& dtc_code,
                                            const VehicleState& freeze_state) {
    common::JsonValue ff = common::JsonValue::object();
    ff["speed_kmh"]  = freeze_state.speed_kmh;
    ff["kl30_mv"]    = freeze_state.kl30_mv;
    ff["bub_mv"]     = freeze_state.bub_mv;
    ff["bub_soh"]    = freeze_state.bub_soh;
    ff["cpu_temp_c"] = static_cast<double>(freeze_state.cpu_temp_c);

    DtcRecord rec;
    rec.dtc_code          = dtc_code;
    rec.status_mask       = 0x09U; // testFailed (0x01) | confirmedDTC (0x08)
    rec.last_seen_utc     = currentEpochSeconds();
    rec.first_seen_utc    = rec.last_seen_utc;
    rec.freeze_frame_json = ff.dump(false);
    return storage_.upsertDtc(rec);
}

std::vector<DtcRecord> PersistentStoreService::getDtcs(uint8_t mask) const {
    return storage_.readAllDtcs(mask);
}

bool PersistentStoreService::clearDtcs() {
    return storage_.clearAllDtcs();
}

bool PersistentStoreService::flushBeforeSleep() {
    return storage_.flushAndCheckpoint();
}

} // namespace tcu::service

