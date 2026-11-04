#pragma once

#include "hal/i_crypto_hal.h"
#include "hal/i_storage_hal.h"

#include <string>
#include <utility>
#include <vector>

namespace tcu::service {

/// High-level encrypted persistence service wrapping SQLite WAL + AES-256-GCM.
class PersistentStoreService {
public:
    PersistentStoreService(hal::IStorageHAL& storage,
                           hal::ICryptoHAL& crypto,
                           const std::string& key_hex);

    /// Encrypt JSON telemetry with AES-256-GCM and store in SQLite offline queue.
    bool storeEncryptedBreadcrumb(int priority,
                                  const std::string& topic,
                                  const std::string& json_payload);

    /// Fetch and decrypt pending offline breadcrumbs in priority order (0=eCall, 1=SVT, 2=Normal).
    struct DecryptedRow {
        int64_t     id{0};
        int         priority{2};
        std::string topic;
        std::string plaintext_json;
    };
    std::vector<DecryptedRow> popOrPeekDecryptedBatch(size_t limit = 32);
    bool acknowledgeDelivered(int64_t id);
    size_t pendingCount() const;

    bool recordFaultDtc(const std::string& dtc_code, const VehicleState& freeze_state);
    std::vector<DtcRecord> getDtcs(uint8_t mask = 0xFF) const;
    bool clearDtcs();

    bool flushBeforeSleep();

private:
    hal::IStorageHAL&    storage_;
    hal::ICryptoHAL&     crypto_;
    std::vector<uint8_t> aes_key_;
};

} // namespace tcu::service

