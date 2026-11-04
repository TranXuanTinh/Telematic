#pragma once

#include "hal/i_storage_hal.h"
#include <mutex>
#include <sqlite3.h>
#include <string>

namespace tcu::hal {

/// Production WAL-Mode SQLite3 Storage HAL implementing LLD Section 6 Schema.
class SqliteStorageHAL : public IStorageHAL {
public:
    SqliteStorageHAL() = default;
    ~SqliteStorageHAL() override;

    bool open(const std::string& db_path, int max_queue_records = 50000) override;
    void close() override;
    bool isHealthy() const override;
    bool flushAndCheckpoint() override;

    bool enqueueTelemetry(int priority,
                          const std::string& topic,
                          const std::vector<uint8_t>& encrypted_payload) override;
    std::vector<QueuedTelemetryRow> fetchPendingTelemetry(size_t limit = 32) override;
    bool removeTelemetryById(int64_t id) override;
    size_t getPendingTelemetryCount() const override;

    bool upsertDtc(const DtcRecord& dtc) override;
    std::vector<DtcRecord> readAllDtcs(uint8_t status_mask_filter = 0xFF) const override;
    bool clearAllDtcs() override;

    bool upsertOtaJournal(const OtaJournalRow& row) override;
    std::optional<OtaJournalRow> getLatestOtaJournal() const override;

private:
    bool execSqlLocked(const char* sql) const;
    void enforceRingBufferLimitLocked();

    mutable std::mutex mtx_;
    sqlite3*           db_{nullptr};
    int                max_records_{50000};
};

} // namespace tcu::hal

