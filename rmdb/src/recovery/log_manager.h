/* Copyright (c) 2023 Renmin University of China */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "log_defs.h"
#include "record/rm_defs.h"

enum LogType : int32_t {
    UPDATE = 0,
    INSERT,
    DELETE,
    begin,
    commit,
    ABORT,
    INDEX_INSERT,
    INDEX_DELETE,
    CREATE_TABLE,
    DROP_TABLE,
    CREATE_INDEX,
    DROP_INDEX
};

/**
 * A single portable log representation is used for every operation.  Empty
 * fields cost only their four-byte length and make validation/recovery much
 * less error prone than a family of unrelated binary layouts.
 */
class LogRecord {
   public:
    LogRecord() = default;
    LogRecord(LogType type, txn_id_t txn_id) : log_type_(type), log_tid_(txn_id) {}
    virtual ~LogRecord() = default;

    void set_object_name(std::string value) { object_name_ = std::move(value); }
    void set_aux_name(std::string value) { aux_name_ = std::move(value); }
    void set_before_image(const char *data, size_t size) { before_image_.assign(data, data + size); }
    void set_after_image(const char *data, size_t size) { after_image_.assign(data, data + size); }
    void set_key(const char *data, size_t size) { key_.assign(data, data + size); }
    void set_metadata(std::string value) { metadata_.assign(value.begin(), value.end()); }
    void set_rid(const Rid &rid) { rid_ = rid; }

    uint32_t serialized_size() const;
    void serialize(char *dest) const;
    static std::unique_ptr<LogRecord> deserialize(const char *src, size_t available);
    static bool valid_type(int32_t type);

    LogType log_type_{begin};
    lsn_t lsn_{INVALID_LSN};       // byte offset in db.log
    uint32_t log_tot_len_{0};
    txn_id_t log_tid_{INVALID_TXN_ID};
    lsn_t prev_lsn_{INVALID_LSN};
    Rid rid_{INVALID_PAGE_ID, -1};
    std::string object_name_;      // table name or index file name
    std::string aux_name_;         // tombstone name for DROP
    std::vector<char> before_image_;
    std::vector<char> after_image_;
    std::vector<char> key_;
    std::vector<char> metadata_;   // db.meta snapshot for DDL
};

class BeginLogRecord : public LogRecord {
   public:
    explicit BeginLogRecord(txn_id_t txn_id) : LogRecord(begin, txn_id) {}
};

class CommitLogRecord : public LogRecord {
   public:
    explicit CommitLogRecord(txn_id_t txn_id) : LogRecord(commit, txn_id) {}
};

class AbortLogRecord : public LogRecord {
   public:
    explicit AbortLogRecord(txn_id_t txn_id) : LogRecord(ABORT, txn_id) {}
};

class InsertLogRecord : public LogRecord {
   public:
    InsertLogRecord(txn_id_t txn_id, const RmRecord &value, const Rid &rid, const std::string &table)
        : LogRecord(INSERT, txn_id) {
        set_object_name(table);
        set_after_image(value.data, value.size);
        set_rid(rid);
    }
};

class DeleteLogRecord : public LogRecord {
   public:
    DeleteLogRecord(txn_id_t txn_id, const RmRecord &value, const Rid &rid, const std::string &table)
        : LogRecord(DELETE, txn_id) {
        set_object_name(table);
        set_before_image(value.data, value.size);
        set_rid(rid);
    }
};

class UpdateLogRecord : public LogRecord {
   public:
    UpdateLogRecord(txn_id_t txn_id, const RmRecord &before, const RmRecord &after, const Rid &rid,
                    const std::string &table)
        : LogRecord(UPDATE, txn_id) {
        set_object_name(table);
        set_before_image(before.data, before.size);
        set_after_image(after.data, after.size);
        set_rid(rid);
    }
};

class LogBuffer {
   public:
    LogBuffer() { reset(); }
    bool is_full(size_t append_size) const { return offset_ + append_size > LOG_BUFFER_SIZE; }
    void reset() {
        offset_ = 0;
        last_lsn_ = INVALID_LSN;
    }

    char buffer_[LOG_BUFFER_SIZE];
    size_t offset_{0};
    lsn_t last_lsn_{INVALID_LSN};
};

class LogManager {
   public:
    explicit LogManager(DiskManager *disk_manager) : disk_manager_(disk_manager) {}

    // Called after chdir(database), because managers are constructed before a database is opened.
    void initialize();
    lsn_t add_log_to_buffer(LogRecord *log_record);
    lsn_t append(LogRecord &log_record, lsn_t prev_lsn);
    void flush_log_to_disk(bool sync = true);
    void flush_up_to(lsn_t lsn);
    std::unique_ptr<LogRecord> read_record(lsn_t lsn);
    std::vector<std::unique_ptr<LogRecord>> read_all_valid(bool truncate_invalid_tail);
    void reset_log();

    lsn_t persistent_lsn() const { return persist_lsn_.load(); }
    bool initialized() const { return initialized_; }
    void set_no_fsync(bool v) { std::lock_guard<std::mutex> guard(latch_); no_fsync_ = v; }

   private:
    void flush_locked(bool sync);

    std::atomic<lsn_t> next_lsn_{0};
    std::atomic<lsn_t> persist_lsn_{INVALID_LSN};
    std::mutex latch_;
    LogBuffer log_buffer_;
    DiskManager *disk_manager_;
    bool initialized_{false};
    bool no_fsync_{true};
};
