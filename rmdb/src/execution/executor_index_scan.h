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

#include <cstring>
#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    std::unique_ptr<RmRecord> current_record_;

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        IxIndexHandle *ih = sm_manager_->ihs_.at(
            sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();
        std::vector<char> lower(index_meta_.col_tot_len);
        std::vector<char> upper(index_meta_.col_tot_len);
        fill_key_extreme(lower.data(), false, 0);
        fill_key_extreme(upper.data(), true, 0);

        bool lower_exclusive = false;
        bool upper_inclusive = true;
        int offset = 0;
        for (size_t col_idx = 0; col_idx < index_meta_.cols.size(); ++col_idx) {
            const ColMeta &col = index_meta_.cols[col_idx];
            const Condition *equal = nullptr;
            const Condition *best_lower = nullptr;
            const Condition *best_upper = nullptr;
            for (const auto &cond : conds_) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ ||
                    cond.lhs_col.col_name != col.name) {
                    continue;
                }
                if (cond.op == OP_EQ) {
                    equal = &cond;
                } else if (cond.op == OP_GT || cond.op == OP_GE) {
                    if (best_lower == nullptr) {
                        best_lower = &cond;
                    } else {
                        int cmp = ix_compare(cond.rhs_val.raw->data, best_lower->rhs_val.raw->data,
                                             col.type, col.len);
                        if (cmp > 0 || (cmp == 0 && cond.op == OP_GT)) {
                            best_lower = &cond;
                        }
                    }
                } else if (cond.op == OP_LT || cond.op == OP_LE) {
                    if (best_upper == nullptr) {
                        best_upper = &cond;
                    } else {
                        int cmp = ix_compare(cond.rhs_val.raw->data, best_upper->rhs_val.raw->data,
                                             col.type, col.len);
                        if (cmp < 0 || (cmp == 0 && cond.op == OP_LT)) {
                            best_upper = &cond;
                        }
                    }
                }
            }

            if (equal != nullptr) {
                memcpy(lower.data() + offset, equal->rhs_val.raw->data, col.len);
                memcpy(upper.data() + offset, equal->rhs_val.raw->data, col.len);
                offset += col.len;
                continue;
            }

            if (best_lower != nullptr) {
                memcpy(lower.data() + offset, best_lower->rhs_val.raw->data, col.len);
                lower_exclusive = best_lower->op == OP_GT;
                fill_key_extreme(lower.data(), lower_exclusive, col_idx + 1);
            }
            if (best_upper != nullptr) {
                memcpy(upper.data() + offset, best_upper->rhs_val.raw->data, col.len);
                upper_inclusive = best_upper->op == OP_LE;
                fill_key_extreme(upper.data(), upper_inclusive, col_idx + 1);
            }
            break;
        }

        Iid begin;
        Iid end;
        std::vector<ColType> key_types;
        std::vector<int> key_lens;
        for (const auto &col : index_meta_.cols) {
            key_types.push_back(col.type);
            key_lens.push_back(col.len);
        }
        if (ix_compare(lower.data(), upper.data(), key_types, key_lens) > 0) {
            begin = ih->leaf_end();
            end = begin;
        } else {
            begin = lower_exclusive ? ih->upper_bound(lower.data()) : ih->lower_bound(lower.data());
            end = upper_inclusive ? ih->upper_bound(upper.data()) : ih->lower_bound(upper.data());
        }
        scan_ = std::make_unique<IxScan>(ih, begin, end, sm_manager_->get_bpm());
        current_record_.reset();
        skip_unmatched();
    }

    void nextTuple() override {
        if (is_end()) {
            return;
        }
        current_record_.reset();
        scan_->next();
        skip_unmatched();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_record_);
    }

    Rid &rid() override { return rid_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }

   private:
    void fill_key_extreme(char *key, bool maximum, size_t begin_col) {
        int offset = 0;
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            const ColMeta &col = index_meta_.cols[i];
            if (i >= begin_col) {
                if (col.type == TYPE_INT) {
                    int value = maximum ? std::numeric_limits<int>::max() : std::numeric_limits<int>::lowest();
                    memcpy(key + offset, &value, sizeof(value));
                } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
                    int64_t value = maximum ? std::numeric_limits<int64_t>::max()
                                            : std::numeric_limits<int64_t>::lowest();
                    memcpy(key + offset, &value, sizeof(value));
                } else if (col.type == TYPE_FLOAT) {
                    float value = maximum ? std::numeric_limits<float>::max()
                                          : std::numeric_limits<float>::lowest();
                    memcpy(key + offset, &value, sizeof(value));
                } else {
                    memset(key + offset, maximum ? 0xff : 0, col.len);
                }
            }
            offset += col.len;
        }
    }

    void skip_unmatched() {
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, rec.get(), fed_conds_)) {
                current_record_ = std::move(rec);
                return;
            }
            scan_->next();
        }
        current_record_.reset();
    }
};
