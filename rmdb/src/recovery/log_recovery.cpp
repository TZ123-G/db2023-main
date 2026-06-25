/* Copyright (c) 2023 Renmin University of China */
#include "log_recovery.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "errors.h"

namespace {

RmFileHandle *table_handle(SmManager *sm_manager, const std::string &name) {
    auto it = sm_manager->fhs_.find(name);
    if (it == sm_manager->fhs_.end()) {
        throw InternalError("Recovery cannot open table " + name);
    }
    return it->second.get();
}

void ensure_value(RmFileHandle *fh, const Rid &rid, const std::vector<char> &value) {
    if (value.empty()) throw InternalError("Recovery log contains an empty tuple image");
    if (fh->is_record(rid)) {
        fh->update_record(rid, const_cast<char *>(value.data()), nullptr);
    } else {
        fh->insert_record(rid, const_cast<char *>(value.data()));
    }
}

DbMeta parse_meta(const std::vector<char> &image) {
    std::string text(image.begin(), image.end());
    std::istringstream input(text);
    DbMeta db;
    input >> db;
    if (!input) throw InternalError("Invalid metadata image in DDL log");
    return db;
}

void write_meta_image(const std::vector<char> &image) {
    std::ofstream out(DB_META_NAME, std::ios::out | std::ios::trunc);
    if (!out.is_open()) throw FileNotFoundError(DB_META_NAME);
    out.write(image.data(), static_cast<std::streamsize>(image.size()));
    out.flush();
}

const IndexMeta *find_index_by_name(SmManager *sm_manager, const DbMeta &db, const std::string &name) {
    for (const auto &tab_entry : db.tables()) {
        for (const auto &index : tab_entry.second.indexes) {
            if (sm_manager->get_ix_manager()->get_index_name(tab_entry.first, index.cols) == name) {
                return &index;
            }
        }
    }
    return nullptr;
}

void destroy_if_exists(DiskManager *disk_manager, const std::string &name) {
    if (disk_manager->is_file(name)) disk_manager->destroy_file(name);
}

void restore_tombstone(DiskManager *disk_manager, const std::string &original,
                       const std::string &tombstone) {
    if (!disk_manager->is_file(original) && disk_manager->is_file(tombstone)) {
        disk_manager->rename_file(tombstone, original);
    }
}

}  // namespace

void RecoveryManager::analyze() {
    records_ = log_manager_->read_all_valid(true);
    active_txns_.clear();
    committed_txns_.clear();
    for (const auto &record : records_) {
        switch (record->log_type_) {
            case begin:
                active_txns_[record->log_tid_] = record->lsn_;
                break;
            case commit:
                committed_txns_.insert(record->log_tid_);
                active_txns_.erase(record->log_tid_);
                break;
            case ABORT:
                // Runtime abort does not force every undone data page. Repeating
                // undo after a crash is therefore required and is idempotent.
                active_txns_[record->log_tid_] = record->lsn_;
                break;
            default:
                active_txns_[record->log_tid_] = record->lsn_;
                break;
        }
    }
}

void RecoveryManager::recover_ddl() {
    auto redo_ddl = [&](const LogRecord &record) {
        DbMeta after = parse_meta(record.after_image_);
        if (record.log_type_ == CREATE_TABLE) {
            if (!disk_manager_->is_file(record.object_name_)) {
                const TabMeta &tab = after.get_table(record.object_name_);
                int record_size = 0;
                for (const auto &col : tab.cols) record_size += col.len;
                sm_manager_->get_rm_manager()->create_file(record.object_name_, record_size);
            }
        } else if (record.log_type_ == CREATE_INDEX) {
            const IndexMeta *index = find_index_by_name(sm_manager_, after, record.object_name_);
            if (index != nullptr && !disk_manager_->is_file(record.object_name_)) {
                sm_manager_->get_ix_manager()->create_index(index->tab_name, index->cols);
            }
        } else if (record.log_type_ == DROP_TABLE) {
            DbMeta before = parse_meta(record.before_image_);
            const TabMeta &tab = before.get_table(record.object_name_);
            destroy_if_exists(disk_manager_, record.object_name_);
            destroy_if_exists(disk_manager_, record.aux_name_ + record.object_name_);
            for (const auto &index : tab.indexes) {
                std::string name = sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols);
                destroy_if_exists(disk_manager_, name);
                destroy_if_exists(disk_manager_, record.aux_name_ + name);
            }
        } else if (record.log_type_ == DROP_INDEX) {
            destroy_if_exists(disk_manager_, record.object_name_);
            destroy_if_exists(disk_manager_, record.aux_name_);
        }
        write_meta_image(record.after_image_);
    };

    auto undo_ddl = [&](const LogRecord &record) {
        DbMeta before = parse_meta(record.before_image_);
        if (record.log_type_ == CREATE_TABLE) {
            destroy_if_exists(disk_manager_, record.object_name_);
        } else if (record.log_type_ == CREATE_INDEX) {
            destroy_if_exists(disk_manager_, record.object_name_);
        } else if (record.log_type_ == DROP_TABLE) {
            const TabMeta &tab = before.get_table(record.object_name_);
            restore_tombstone(disk_manager_, record.object_name_,
                              record.aux_name_ + record.object_name_);
            for (const auto &index : tab.indexes) {
                std::string name = sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols);
                restore_tombstone(disk_manager_, name, record.aux_name_ + name);
            }
        } else if (record.log_type_ == DROP_INDEX) {
            restore_tombstone(disk_manager_, record.object_name_, record.aux_name_);
        }
        write_meta_image(record.before_image_);
    };

    for (const auto &record : records_) {
        if (record->log_type_ < CREATE_TABLE || record->log_type_ > DROP_INDEX) continue;
        if (committed_txns_.count(record->log_tid_) != 0) redo_ddl(*record);
    }
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if ((*it)->log_type_ < CREATE_TABLE || (*it)->log_type_ > DROP_INDEX) continue;
        if (active_txns_.count((*it)->log_tid_) != 0) undo_ddl(**it);
    }
}

void RecoveryManager::redo_record(const LogRecord &record) {
    if (record.log_type_ == INSERT) {
        ensure_value(table_handle(sm_manager_, record.object_name_), record.rid_, record.after_image_);
    } else if (record.log_type_ == DELETE) {
        RmFileHandle *fh = table_handle(sm_manager_, record.object_name_);
        if (fh->is_record(record.rid_)) fh->delete_record(record.rid_, nullptr);
    } else if (record.log_type_ == UPDATE) {
        ensure_value(table_handle(sm_manager_, record.object_name_), record.rid_, record.after_image_);
    }
    // Index records are intentionally not replayed into a possibly half-split
    // B+ tree.  All indexes are rebuilt from recovered base tables below.
}

void RecoveryManager::redo() {
    for (const auto &record : records_) {
        if (committed_txns_.count(record->log_tid_) != 0) {
            redo_record(*record);
        }
    }
}

void RecoveryManager::undo_record(const LogRecord &record) {
    if (record.log_type_ == INSERT) {
        RmFileHandle *fh = table_handle(sm_manager_, record.object_name_);
        if (fh->is_record(record.rid_)) fh->delete_record(record.rid_, nullptr);
    } else if (record.log_type_ == DELETE) {
        ensure_value(table_handle(sm_manager_, record.object_name_), record.rid_, record.before_image_);
    } else if (record.log_type_ == UPDATE) {
        ensure_value(table_handle(sm_manager_, record.object_name_), record.rid_, record.before_image_);
    }
}

void RecoveryManager::undo() {
    if (!active_txns_.empty()) {
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
            if (active_txns_.count((*it)->log_tid_) != 0) {
                undo_record(**it);
            }
        }
    }
    sm_manager_->rebuild_indexes();
    sm_manager_->flush_recovery_state();
    log_manager_->reset_log();
    records_.clear();
    active_txns_.clear();
    committed_txns_.clear();
}
