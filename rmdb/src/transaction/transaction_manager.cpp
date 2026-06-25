/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include <cstring>
#include <vector>

#include "errors.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {

IxIndexHandle *get_index_handle(SmManager *sm_manager, const std::string &tab_name, const IndexMeta &index) {
    auto ix_name = sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols);
    auto ih_it = sm_manager->ihs_.find(ix_name);
    if (ih_it == sm_manager->ihs_.end()) {
        throw InternalError("Index " + ix_name + " not loaded for table " + tab_name);
    }
    return ih_it->second.get();
}

std::vector<char> build_index_key(const IndexMeta &index, const char *record_data) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (const auto &col : index.cols) {
        memcpy(key.data() + offset, record_data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

void ensure_index_entry(SmManager *sm_manager, const std::string &tab_name, const IndexMeta &index,
                        const char *record_data, const Rid &rid, Transaction *txn) {
    auto key = build_index_key(index, record_data);
    IxIndexHandle *ih = get_index_handle(sm_manager, tab_name, index);
    std::vector<Rid> result;
    if (ih->get_value(key.data(), &result, txn)) {
        if (!result.empty() && result.front() == rid) {
            return;
        }
        throw UniqueConstraintError();
    }
    ih->insert_entry(key.data(), rid, txn);
}

void clear_write_set(Transaction *txn) {
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }
}

void release_locks(Transaction *txn, LockManager *lock_manager) {
    auto lock_set = txn->get_lock_set();
    std::vector<LockDataId> locks(lock_set->begin(), lock_set->end());
    for (const auto &lock_data_id : locks) {
        lock_manager->unlock(txn, lock_data_id);
    }
    lock_set->clear();
}

}  // namespace

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    txn->set_state(TransactionState::GROWING);
    txn->set_start_ts(next_timestamp_++);
    if (log_manager != nullptr) {
        BeginLogRecord log(txn->get_transaction_id());
        txn->set_prev_lsn(log_manager->append(log, INVALID_LSN));
    }

    std::unique_lock<std::mutex> lock(latch_);
    TransactionManager::txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED ||
        txn->get_state() == TransactionState::ABORTED) {
        return;
    }

    if (log_manager != nullptr) {
        CommitLogRecord log(txn->get_transaction_id());
        lsn_t lsn = log_manager->append(log, txn->get_prev_lsn());
        txn->set_prev_lsn(lsn);
        // A commit is acknowledged only after its complete log record reaches
        // stable storage.
        log_manager->flush_log_to_disk(true);
        try {
            sm_manager_->finalize_ddl(txn, log_manager);
        } catch (...) {
            // Tombstones are recovery metadata. A cleanup failure must not
            // turn an already durable commit into an abort.
        }
    }
    clear_write_set(txn);
    release_locks(txn, lock_manager_);
    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED ||
        txn->get_state() == TransactionState::ABORTED) {
        return;
    }

    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        WriteRecord *write_record = write_set->back();
        const std::string &tab_name = write_record->GetTableName();
        const Rid &rid = write_record->GetRid();
        TabMeta &tab = sm_manager_->db_.get_table(tab_name);
        RmFileHandle *fh = sm_manager_->fhs_.at(tab_name).get();

        if (write_record->GetWriteType() == WType::UPDATE_TUPLE) {
            uint64_t write_group_id = write_record->GetWriteGroupId();
            std::vector<WriteRecord *> update_group;
            for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
                if ((*it)->GetWriteType() != WType::UPDATE_TUPLE ||
                    (*it)->GetWriteGroupId() != write_group_id) {
                    break;
                }
                update_group.push_back(*it);
            }

            for (WriteRecord *update_record : update_group) {
                auto current_record = fh->get_record(update_record->GetRid(), nullptr);
                for (const auto &index : tab.indexes) {
                    auto key = build_index_key(index, current_record->data);
                    get_index_handle(sm_manager_, tab_name, index)->delete_entry(key.data(), txn);
                }
            }
            for (WriteRecord *update_record : update_group) {
                RmRecord &old_record = update_record->GetRecord();
                fh->update_record(update_record->GetRid(), old_record.data, nullptr);
            }
            for (WriteRecord *update_record : update_group) {
                RmRecord &old_record = update_record->GetRecord();
                for (const auto &index : tab.indexes) {
                    ensure_index_entry(sm_manager_, tab_name, index, old_record.data,
                                       update_record->GetRid(), txn);
                }
            }
            for (size_t i = 0; i < update_group.size(); ++i) {
                delete write_set->back();
                write_set->pop_back();
            }
            continue;
        }

        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                auto record = fh->get_record(rid, nullptr);
                for (const auto &index : tab.indexes) {
                    auto key = build_index_key(index, record->data);
                    if (!get_index_handle(sm_manager_, tab_name, index)->delete_entry(key.data(), txn)) {
                        throw InternalError("Failed to undo inserted index entry");
                    }
                }
                fh->delete_record(rid, nullptr);
                break;
            }
            case WType::DELETE_TUPLE: {
                RmRecord &old_record = write_record->GetRecord();
                if (fh->is_record(rid)) {
                    fh->update_record(rid, old_record.data, nullptr);
                } else {
                    fh->insert_record(rid, old_record.data);
                }
                for (const auto &index : tab.indexes) {
                    ensure_index_entry(sm_manager_, tab_name, index, old_record.data, rid, txn);
                }
                break;
            }
            case WType::UPDATE_TUPLE:
                break;
        }

        delete write_record;
        write_set->pop_back();
    }

    if (log_manager != nullptr) {
        sm_manager_->rollback_ddl(txn, log_manager);
        // We do not emit ARIES compensation log records.  Therefore all undo
        // effects must be on disk before ABORT can mark the transaction as
        // complete.  If a crash happens earlier, no ABORT exists and startup
        // safely repeats the idempotent undo.
        sm_manager_->flush_abort_state();
        AbortLogRecord log(txn->get_transaction_id());
        lsn_t lsn = log_manager->append(log, txn->get_prev_lsn());
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk(true);
    }

    release_locks(txn, lock_manager_);
    txn->set_state(TransactionState::ABORTED);
}
