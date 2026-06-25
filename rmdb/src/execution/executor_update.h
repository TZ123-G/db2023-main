/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include <string>
#include <unordered_set>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index_key.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        // 可串行化隔离：扫描阶段的 S 锁与 IX 合并为 SIX
        context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        // 先一次性获取全部目标记录的 X 锁，再读取旧值、维护索引和修改记录
        for (const auto &rid : rids_) {
            context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd());
        }

        auto get_ih = [this](const IndexMeta &index) -> IxIndexHandle * {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            auto ih_it = sm_manager_->ihs_.find(ix_name);
            if (ih_it == sm_manager_->ihs_.end()) {
                throw InternalError("Index " + ix_name + " not loaded for table " + tab_name_);
            }
            return ih_it->second.get();
        };

        struct PendingUpdate {
            Rid rid;
            std::unique_ptr<RmRecord> old_record;
            std::unique_ptr<RmRecord> new_record;
            std::vector<std::vector<char>> old_keys;
            std::vector<std::vector<char>> new_keys;
            std::vector<bool> changed_indexes;
        };

        std::vector<PendingUpdate> pending;
        pending.reserve(rids_.size());
        for (const auto &rid : rids_) {
            PendingUpdate item;
            item.rid = rid;
            item.old_record = fh_->get_record(rid, context_);
            item.new_record = std::make_unique<RmRecord>(*item.old_record);
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                if (set_clause.rhs.raw == nullptr) {
                    set_clause.rhs.init_raw(col->len);
                }
                memcpy(item.new_record->data + col->offset, set_clause.rhs.raw->data, col->len);
            }
            for (const auto &index : tab_.indexes) {
                auto old_key = build_index_key(index, item.old_record->data);
                auto new_key = build_index_key(index, item.new_record->data);
                item.changed_indexes.push_back(old_key != new_key);
                item.old_keys.push_back(std::move(old_key));
                item.new_keys.push_back(std::move(new_key));
            }
            pending.push_back(std::move(item));
        }

        for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
            bool any_changed = std::any_of(pending.begin(), pending.end(), [&](const PendingUpdate &item) {
                return item.changed_indexes[index_no];
            });
            if (!any_changed) {
                continue;
            }
            const auto &index = tab_.indexes[index_no];
            std::unordered_set<std::string> new_keys;
            auto ih = get_ih(index);
            for (const auto &item : pending) {
                const auto &key = item.new_keys[index_no];
                std::string key_string(key.data(), key.size());
                if (!new_keys.insert(key_string).second) {
                    throw UniqueConstraintError();
                }
                if (!item.changed_indexes[index_no]) {
                    continue;
                }
                std::vector<Rid> existing;
                if (ih->get_value(key.data(), &existing, context_->txn_)) {
                    const Rid &found = existing.front();
                    auto target = std::find_if(pending.begin(), pending.end(), [&](const PendingUpdate &candidate) {
                        return candidate.rid == found;
                    });
                    if (found != item.rid &&
                        (target == pending.end() || !target->changed_indexes[index_no])) {
                        throw UniqueConstraintError();
                    }
                }
            }
        }

        uint64_t write_group_id = context_->txn_->next_write_group_id();
        for (const auto &item : pending) {
            context_->txn_->append_write_record(
                new WriteRecord(WType::UPDATE_TUPLE, tab_name_, item.rid, *item.old_record, write_group_id));
        }

        for (auto &item : pending) {
            for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
                if (!item.changed_indexes[index_no]) {
                    continue;
                }
                const auto &index = tab_.indexes[index_no];
                auto ih = get_ih(index);
                ih->delete_entry(item.old_keys[index_no].data(), context_->txn_);
            }
        }
        for (auto &item : pending) {
            fh_->update_record(item.rid, item.new_record->data, context_);
        }
        for (auto &item : pending) {
            for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
                if (!item.changed_indexes[index_no]) {
                    continue;
                }
                const auto &index = tab_.indexes[index_no];
                auto ih = get_ih(index);
                ih->insert_entry(item.new_keys[index_no].data(), item.rid, context_->txn_);
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
