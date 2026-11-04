#include "hal/sa515m/sqlite_storage_hal.h"

#include <filesystem>

namespace tcu::hal {

SqliteStorageHAL::~SqliteStorageHAL() {
    close();
}

bool SqliteStorageHAL::execSqlLocked(const char* sql) const {
    if (!db_) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (err) {
        sqlite3_free(err);
    }
    return rc == SQLITE_OK;
}

bool SqliteStorageHAL::open(const std::string& db_path, int max_queue_records) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    max_records_ = (max_queue_records > 0) ? max_queue_records : 50000;

    std::error_code ec;
    auto parent = std::filesystem::path(db_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    if (sqlite3_open_v2(
            db_path.c_str(),
            &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
        db_ = nullptr;
        return false;
    }

    sqlite3_busy_timeout(db_, 2000);
    execSqlLocked("PRAGMA journal_mode=WAL;");
    execSqlLocked("PRAGMA synchronous=NORMAL;");

    const char* kSchemaSql = R"SQL(
        CREATE TABLE IF NOT EXISTS telemetry_queue (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            priority INTEGER NOT NULL,
            created_utc INTEGER NOT NULL,
            topic TEXT NOT NULL,
            encrypted_payload BLOB NOT NULL,
            retry_count INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_telem_prio_time ON telemetry_queue(priority ASC, id ASC);

        CREATE TABLE IF NOT EXISTS dtc_records (
            dtc_code TEXT PRIMARY KEY,
            status_mask INTEGER NOT NULL,
            first_seen_utc INTEGER NOT NULL,
            last_seen_utc INTEGER NOT NULL,
            occurrence_count INTEGER NOT NULL DEFAULT 1,
            freeze_frame_json TEXT
        );

        CREATE TABLE IF NOT EXISTS ota_journal (
            campaign_id TEXT PRIMARY KEY,
            target_version TEXT NOT NULL,
            previous_slot TEXT NOT NULL,
            target_slot TEXT NOT NULL,
            state TEXT NOT NULL,
            bytes_downloaded INTEGER DEFAULT 0,
            updated_utc INTEGER NOT NULL,
            failure_reason TEXT
        );
    )SQL";

    return execSqlLocked(kSchemaSql);
}

void SqliteStorageHAL::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (db_) {
        sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SqliteStorageHAL::isHealthy() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return execSqlLocked("SELECT 1;");
}

bool SqliteStorageHAL::flushAndCheckpoint() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return false;
    return sqlite3_wal_checkpoint_v2(
               db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr) == SQLITE_OK;
}

void SqliteStorageHAL::enforceRingBufferLimitLocked() {
    sqlite3_stmt* count_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM telemetry_queue;", -1, &count_stmt, nullptr) !=
        SQLITE_OK) {
        return;
    }
    int total = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) {
        total = sqlite3_column_int(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);

    if (total < max_records_) {
        return;
    }

    // Evict normal-priority (priority >= 2) oldest records first, preserving SVT/eCall breadcrumbs
    int excess = (total - max_records_) + 1;
    sqlite3_stmt* del_stmt = nullptr;
    const char* kDelNormal =
        "DELETE FROM telemetry_queue WHERE id IN ("
        "SELECT id FROM telemetry_queue WHERE priority >= 2 ORDER BY id ASC LIMIT ?1);";
    if (sqlite3_prepare_v2(db_, kDelNormal, -1, &del_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(del_stmt, 1, excess);
        sqlite3_step(del_stmt);
        int deleted = sqlite3_changes(db_);
        sqlite3_finalize(del_stmt);
        excess -= deleted;
    }

    if (excess > 0) {
        const char* kDelOldest =
            "DELETE FROM telemetry_queue WHERE id IN ("
            "SELECT id FROM telemetry_queue ORDER BY priority DESC, id ASC LIMIT ?1);";
        if (sqlite3_prepare_v2(db_, kDelOldest, -1, &del_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(del_stmt, 1, excess);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }
    }
}

bool SqliteStorageHAL::enqueueTelemetry(int priority,
                                        const std::string& topic,
                                        const std::vector<uint8_t>& encrypted_payload) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return false;

    enforceRingBufferLimitLocked();

    const char* sql =
        "INSERT INTO telemetry_queue (priority, created_utc, topic, encrypted_payload, retry_count)"
        " VALUES (?1, ?2, ?3, ?4, 0);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(stmt, 1, priority);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(currentEpochSeconds()));
    sqlite3_bind_text(stmt, 3, topic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt,
                      4,
                      encrypted_payload.empty() ? "" : reinterpret_cast<const void*>(encrypted_payload.data()),
                      static_cast<int>(encrypted_payload.size()),
                      SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<QueuedTelemetryRow> SqliteStorageHAL::fetchPendingTelemetry(size_t limit) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<QueuedTelemetryRow> rows;
    if (!db_) return rows;

    const char* sql =
        "SELECT id, priority, created_utc, topic, encrypted_payload, retry_count "
        "FROM telemetry_queue ORDER BY priority ASC, id ASC LIMIT ?1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int(stmt, 1, static_cast<int>(limit));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QueuedTelemetryRow r;
        r.id          = sqlite3_column_int64(stmt, 0);
        r.priority    = sqlite3_column_int(stmt, 1);
        r.created_utc = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
        const unsigned char* t = sqlite3_column_text(stmt, 3);
        if (t) r.topic = reinterpret_cast<const char*>(t);

        const void* blob = sqlite3_column_blob(stmt, 4);
        int blob_len     = sqlite3_column_bytes(stmt, 4);
        if (blob && blob_len > 0) {
            const auto* p = static_cast<const uint8_t*>(blob);
            r.encrypted_payload.assign(p, p + blob_len);
        }
        r.retry_count = sqlite3_column_int(stmt, 5);
        rows.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return rows;
}

bool SqliteStorageHAL::removeTelemetryById(int64_t id) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM telemetry_queue WHERE id = ?1;", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

size_t SqliteStorageHAL::getPendingTelemetryCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM telemetry_queue;", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        return 0;
    }
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return count;
}

bool SqliteStorageHAL::upsertDtc(const DtcRecord& dtc) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return false;

    const char* sql = R"SQL(
        INSERT INTO dtc_records
            (dtc_code, status_mask, first_seen_utc, last_seen_utc, occurrence_count, freeze_frame_json)
        VALUES (?1, ?2, ?3, ?4, 1, ?5)
        ON CONFLICT(dtc_code) DO UPDATE SET
            status_mask = excluded.status_mask,
            last_seen_utc = excluded.last_seen_utc,
            occurrence_count = dtc_records.occurrence_count + 1,
            freeze_frame_json = excluded.freeze_frame_json;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    uint64_t now = (dtc.last_seen_utc != 0) ? dtc.last_seen_utc : currentEpochSeconds();
    uint64_t first = (dtc.first_seen_utc != 0) ? dtc.first_seen_utc : now;

    sqlite3_bind_text(stmt, 1, dtc.dtc_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, dtc.status_mask);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(first));
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(now));
    sqlite3_bind_text(stmt, 5, dtc.freeze_frame_json.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<DtcRecord> SqliteStorageHAL::readAllDtcs(uint8_t status_mask_filter) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<DtcRecord> out;
    if (!db_) return out;

    const char* sql =
        "SELECT dtc_code, status_mask, first_seen_utc, last_seen_utc, occurrence_count, freeze_frame_json "
        "FROM dtc_records ORDER BY dtc_code ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        uint8_t sm = static_cast<uint8_t>(sqlite3_column_int(stmt, 1));
        if (status_mask_filter != 0xFF && (sm & status_mask_filter) == 0U) {
            continue;
        }
        DtcRecord r;
        const unsigned char* c0 = sqlite3_column_text(stmt, 0);
        if (c0) r.dtc_code = reinterpret_cast<const char*>(c0);
        r.status_mask      = sm;
        r.first_seen_utc   = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
        r.last_seen_utc    = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        r.occurrence_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 4));
        const unsigned char* c5 = sqlite3_column_text(stmt, 5);
        if (c5) r.freeze_frame_json = reinterpret_cast<const char*>(c5);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool SqliteStorageHAL::clearAllDtcs() {
    std::lock_guard<std::mutex> lk(mtx_);
    return execSqlLocked("DELETE FROM dtc_records;");
}

bool SqliteStorageHAL::upsertOtaJournal(const OtaJournalRow& row) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return false;

    const char* sql = R"SQL(
        INSERT INTO ota_journal
            (campaign_id, target_version, previous_slot, target_slot, state,
             bytes_downloaded, updated_utc, failure_reason)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(campaign_id) DO UPDATE SET
            target_version = excluded.target_version,
            previous_slot = excluded.previous_slot,
            target_slot = excluded.target_slot,
            state = excluded.state,
            bytes_downloaded = excluded.bytes_downloaded,
            updated_utc = excluded.updated_utc,
            failure_reason = excluded.failure_reason;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    uint64_t ts = (row.updated_utc != 0) ? row.updated_utc : currentEpochSeconds();
    sqlite3_bind_text(stmt, 1, row.campaign_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, row.target_version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, row.previous_slot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, row.target_slot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, row.state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(row.bytes_downloaded));
    sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(ts));
    sqlite3_bind_text(stmt, 8, row.failure_reason.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<OtaJournalRow> SqliteStorageHAL::getLatestOtaJournal() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return std::nullopt;

    const char* sql =
        "SELECT campaign_id, target_version, previous_slot, target_slot, state, "
        "bytes_downloaded, updated_utc, failure_reason "
        "FROM ota_journal ORDER BY updated_utc DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    std::optional<OtaJournalRow> res;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        OtaJournalRow r;
        auto colStr = [&](int i) -> std::string {
            const unsigned char* s = sqlite3_column_text(stmt, i);
            return s ? reinterpret_cast<const char*>(s) : "";
        };
        r.campaign_id      = colStr(0);
        r.target_version   = colStr(1);
        r.previous_slot    = colStr(2);
        r.target_slot      = colStr(3);
        r.state            = colStr(4);
        r.bytes_downloaded = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
        r.updated_utc      = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
        r.failure_reason   = colStr(7);
        res = std::move(r);
    }
    sqlite3_finalize(stmt);
    return res;
}

} // namespace tcu::hal

