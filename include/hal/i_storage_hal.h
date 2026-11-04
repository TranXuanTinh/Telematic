#pragma once

#include "common/types.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tcu::hal {

struct QueuedTelemetryRow {
    int64_t              id{0};
    int                  priority{2}; // 0=eCall, 1=SVT_Alarm, 2=Normal
    uint64_t             created_utc{0};
    std::string          topic;
    std::vector<uint8_t> encrypted_payload;
    int                  retry_count{0};
};

struct OtaJournalRow {
    std::string campaign_id;
    std::string target_version;
    std::string previous_slot;
    std::string target_slot;
    std::string state;
    uint64_t    bytes_downloaded{0};
    uint64_t    updated_utc{0};
    std::string failure_reason;
};

/// Pure Virtual HAL for Persistent WAL-Mode SQLite3 Storage.
class IStorageHAL {
public:
    virtual ~IStorageHAL() = default;

    virtual bool open(const std::string& db_path, int max_queue_records = 50000) = 0;
    virtual void close() = 0;
    virtual bool isHealthy() const = 0;

    /// Flush WAL frames cleanly to disk before TCU enters Suspend/Deep Sleep.
    virtual bool flushAndCheckpoint() = 0;

    // 1. Store-and-Forward Telemetry Queue
    virtual bool enqueueTelemetry(int priority,
                                  const std::string& topic,
                                  const std::vector<uint8_t>& encrypted_payload) = 0;
    virtual std::vector<QueuedTelemetryRow> fetchPendingTelemetry(size_t limit = 32) = 0;
    virtual bool removeTelemetryById(int64_t id) = 0;
    virtual size_t getPendingTelemetryCount() const = 0;

    // 2. ISO 14229 Diagnostic Trouble Codes (DTCs)
    virtual bool upsertDtc(const DtcRecord& dtc) = 0;
    virtual std::vector<DtcRecord> readAllDtcs(uint8_t status_mask_filter = 0xFF) const = 0;
    virtual bool clearAllDtcs() = 0;

    // 3. UN R156 OTA Audit & Recovery Journal
    virtual bool upsertOtaJournal(const OtaJournalRow& row) = 0;
    virtual std::optional<OtaJournalRow> getLatestOtaJournal() const = 0;
};

} // namespace tcu::hal

