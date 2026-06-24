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

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    struct MaterializedTuple {
        RmRecord record;
        Rid rid;

        MaterializedTuple(RmRecord record_, Rid rid_) : record(std::move(record_)), rid(rid_) {}
    };

    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<OrderByClause> order_bys_;
    std::vector<ColMeta> sort_cols_;
    bool has_limit_;
    size_t limit_;
    std::vector<MaterializedTuple> tuples_;
    size_t cursor_ = 0;
    Rid current_rid_{};

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<OrderByClause> order_bys, bool has_limit,
                 size_t limit)
        : prev_(std::move(prev)),
          cols_(prev_->cols()),
          len_(prev_->tupleLen()),
          order_bys_(std::move(order_bys)),
          has_limit_(has_limit),
          limit_(limit) {
        for (const auto &order_by : order_bys_) {
            sort_cols_.push_back(prev_->get_col_offset(order_by.col));
        }
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override {
        tuples_.clear();
        cursor_ = 0;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto record = prev_->Next();
            if (record != nullptr) {
                tuples_.emplace_back(std::move(*record), prev_->rid());
            }
        }
        if (!order_bys_.empty()) {
            std::stable_sort(tuples_.begin(), tuples_.end(), [&](const MaterializedTuple &lhs, const MaterializedTuple &rhs) {
                for (size_t i = 0; i < sort_cols_.size(); ++i) {
                    const auto &col = sort_cols_[i];
                    int cmp = compare(col.type, col.len, lhs.record.data + col.offset, rhs.record.data + col.offset);
                    if (cmp == 0) {
                        continue;
                    }
                    return order_bys_[i].is_desc ? cmp > 0 : cmp < 0;
                }
                return false;
            });
        }
        if (has_limit_ && tuples_.size() > limit_) {
            tuples_.erase(tuples_.begin() + limit_, tuples_.end());
        }
        if (!tuples_.empty()) {
            current_rid_ = tuples_[0].rid;
        }
    }

    void nextTuple() override {
        if (is_end()) {
            return;
        }
        ++cursor_;
        if (!is_end()) {
            current_rid_ = tuples_[cursor_].rid;
        }
    }

    bool is_end() const override { return cursor_ >= tuples_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(tuples_[cursor_].record);
    }

    Rid &rid() override { return current_rid_; }
};
