/* Copyright (c) 2023 Renmin University of China */
#include "log_manager.h"

#include <cstring>
#include <limits>

#include "errors.h"

namespace {

constexpr uint32_t FIELD_COUNT = 6;
constexpr uint32_t FIXED_BODY_SIZE = sizeof(Rid) + FIELD_COUNT * sizeof(uint32_t);

void write_blob(char *dest, size_t &offset, const char *data, uint32_t size) {
    memcpy(dest + offset, &size, sizeof(size));
    offset += sizeof(size);
    if (size != 0) {
        memcpy(dest + offset, data, size);
        offset += size;
    }
}

bool read_blob(const char *src, size_t total, size_t &offset, std::vector<char> *out) {
    if (offset + sizeof(uint32_t) > total) return false;
    uint32_t size;
    memcpy(&size, src + offset, sizeof(size));
    offset += sizeof(size);
    if (size > total - offset) return false;
    out->assign(src + offset, src + offset + size);
    offset += size;
    return true;
}

}  // namespace

bool LogRecord::valid_type(int32_t type) {
    return type >= UPDATE && type <= DROP_INDEX;
}

uint32_t LogRecord::serialized_size() const {
    uint64_t size = LOG_HEADER_SIZE + FIXED_BODY_SIZE + object_name_.size() + aux_name_.size() +
                    before_image_.size() + after_image_.size() + key_.size() + metadata_.size();
    if (size > std::numeric_limits<uint32_t>::max()) {
        throw InternalError("Log record is too large");
    }
    return static_cast<uint32_t>(size);
}

void LogRecord::serialize(char *dest) const {
    int32_t type = static_cast<int32_t>(log_type_);
    memcpy(dest + OFFSET_LOG_TYPE, &type, sizeof(type));
    memcpy(dest + OFFSET_LSN, &lsn_, sizeof(lsn_));
    memcpy(dest + OFFSET_LOG_TOT_LEN, &log_tot_len_, sizeof(log_tot_len_));
    memcpy(dest + OFFSET_LOG_TID, &log_tid_, sizeof(log_tid_));
    memcpy(dest + OFFSET_PREV_LSN, &prev_lsn_, sizeof(prev_lsn_));

    size_t offset = OFFSET_LOG_DATA;
    memcpy(dest + offset, &rid_, sizeof(rid_));
    offset += sizeof(rid_);
    write_blob(dest, offset, object_name_.data(), static_cast<uint32_t>(object_name_.size()));
    write_blob(dest, offset, aux_name_.data(), static_cast<uint32_t>(aux_name_.size()));
    write_blob(dest, offset, before_image_.data(), static_cast<uint32_t>(before_image_.size()));
    write_blob(dest, offset, after_image_.data(), static_cast<uint32_t>(after_image_.size()));
    write_blob(dest, offset, key_.data(), static_cast<uint32_t>(key_.size()));
    write_blob(dest, offset, metadata_.data(), static_cast<uint32_t>(metadata_.size()));
}

std::unique_ptr<LogRecord> LogRecord::deserialize(const char *src, size_t available) {
    if (available < LOG_HEADER_SIZE) return nullptr;
    int32_t type;
    uint32_t total;
    memcpy(&type, src + OFFSET_LOG_TYPE, sizeof(type));
    memcpy(&total, src + OFFSET_LOG_TOT_LEN, sizeof(total));
    if (!valid_type(type) || total < LOG_HEADER_SIZE + FIXED_BODY_SIZE || total > available) return nullptr;

    auto record = std::make_unique<LogRecord>();
    record->log_type_ = static_cast<LogType>(type);
    memcpy(&record->lsn_, src + OFFSET_LSN, sizeof(record->lsn_));
    record->log_tot_len_ = total;
    memcpy(&record->log_tid_, src + OFFSET_LOG_TID, sizeof(record->log_tid_));
    memcpy(&record->prev_lsn_, src + OFFSET_PREV_LSN, sizeof(record->prev_lsn_));
    size_t offset = OFFSET_LOG_DATA;
    memcpy(&record->rid_, src + offset, sizeof(record->rid_));
    offset += sizeof(record->rid_);

    std::vector<char> object;
    std::vector<char> aux;
    if (!read_blob(src, total, offset, &object) || !read_blob(src, total, offset, &aux) ||
        !read_blob(src, total, offset, &record->before_image_) ||
        !read_blob(src, total, offset, &record->after_image_) ||
        !read_blob(src, total, offset, &record->key_) ||
        !read_blob(src, total, offset, &record->metadata_) || offset != total) {
        return nullptr;
    }
    record->object_name_.assign(object.begin(), object.end());
    record->aux_name_.assign(aux.begin(), aux.end());
    return record;
}

void LogManager::initialize() {
    std::lock_guard<std::mutex> guard(latch_);
    int size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (size < 0) {
        throw FileNotFoundError(LOG_FILE_NAME);
    }
    next_lsn_.store(size);
    persist_lsn_.store(size == 0 ? INVALID_LSN : size - 1);
    log_buffer_.reset();
    initialized_ = true;
}

lsn_t LogManager::append(LogRecord &log_record, lsn_t prev_lsn) {
    log_record.prev_lsn_ = prev_lsn;
    return add_log_to_buffer(&log_record);
}

lsn_t LogManager::add_log_to_buffer(LogRecord *log_record) {
    if (log_record == nullptr) throw InternalError("Cannot append null log record");
    std::lock_guard<std::mutex> guard(latch_);
    if (!initialized_) throw InternalError("LogManager is not initialized");

    log_record->log_tot_len_ = log_record->serialized_size();
    if (log_record->log_tot_len_ > LOG_BUFFER_SIZE) {
        throw InternalError("Single log record exceeds log buffer");
    }
    if (log_buffer_.is_full(log_record->log_tot_len_)) {
        flush_locked(false);
    }
    lsn_t lsn = next_lsn_.load();
    log_record->lsn_ = lsn;
    log_record->serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += log_record->log_tot_len_;
    log_buffer_.last_lsn_ = lsn;
    next_lsn_.store(lsn + static_cast<lsn_t>(log_record->log_tot_len_));
    return lsn;
}

void LogManager::flush_locked(bool sync) {
    if (log_buffer_.offset_ != 0) {
        disk_manager_->write_log(log_buffer_.buffer_, static_cast<int>(log_buffer_.offset_));
        persist_lsn_.store(log_buffer_.last_lsn_);
        log_buffer_.reset();
    }
    if (sync) disk_manager_->sync_log();
}

void LogManager::flush_log_to_disk(bool sync) {
    std::lock_guard<std::mutex> guard(latch_);
    if (!initialized_) return;
    flush_locked(sync);
}

void LogManager::flush_up_to(lsn_t lsn) {
    if (lsn == INVALID_LSN || persist_lsn_.load() >= lsn) return;
    std::lock_guard<std::mutex> guard(latch_);
    if (persist_lsn_.load() < lsn) flush_locked(false);
}

std::unique_ptr<LogRecord> LogManager::read_record(lsn_t lsn) {
    if (lsn < 0) return nullptr;
    std::lock_guard<std::mutex> guard(latch_);
    char header[LOG_HEADER_SIZE];
    if (disk_manager_->read_log(header, sizeof(header), lsn) != static_cast<int>(sizeof(header))) return nullptr;
    uint32_t total;
    memcpy(&total, header + OFFSET_LOG_TOT_LEN, sizeof(total));
    if (total < LOG_HEADER_SIZE + FIXED_BODY_SIZE) return nullptr;
    std::vector<char> data(total);
    if (disk_manager_->read_log(data.data(), total, lsn) != static_cast<int>(total)) return nullptr;
    auto record = LogRecord::deserialize(data.data(), data.size());
    if (record == nullptr || record->lsn_ != lsn) return nullptr;
    return record;
}

std::vector<std::unique_ptr<LogRecord>> LogManager::read_all_valid(bool truncate_invalid_tail) {
    flush_log_to_disk(true);
    std::vector<std::unique_ptr<LogRecord>> records;
    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    int offset = 0;
    while (offset + LOG_HEADER_SIZE <= file_size) {
        char header[LOG_HEADER_SIZE];
        if (disk_manager_->read_log(header, sizeof(header), offset) != static_cast<int>(sizeof(header))) break;
        uint32_t total;
        memcpy(&total, header + OFFSET_LOG_TOT_LEN, sizeof(total));
        if (total < LOG_HEADER_SIZE + FIXED_BODY_SIZE ||
            total > static_cast<uint32_t>(file_size - offset)) {
            break;
        }
        std::vector<char> data(total);
        if (disk_manager_->read_log(data.data(), total, offset) != static_cast<int>(total)) break;
        auto record = LogRecord::deserialize(data.data(), data.size());
        if (record == nullptr || record->lsn_ != offset) break;
        offset += static_cast<int>(total);
        records.push_back(std::move(record));
    }
    if (truncate_invalid_tail && offset != file_size) {
        disk_manager_->truncate_log(offset);
    }
    next_lsn_.store(offset);
    persist_lsn_.store(offset == 0 ? INVALID_LSN : offset - 1);
    return records;
}

void LogManager::reset_log() {
    std::lock_guard<std::mutex> guard(latch_);
    log_buffer_.reset();
    disk_manager_->truncate_log(0);
    disk_manager_->sync_log();
    next_lsn_.store(0);
    persist_lsn_.store(INVALID_LSN);
}
