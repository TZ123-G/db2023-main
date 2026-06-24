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
#include <cstring>
#include <memory>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t DEFAULT_JOIN_BUFFER_SIZE = 64 * 1024 * 1024;

    enum class TupleSide { LEFT, RIGHT, VALUE };

    struct BoundOperand {
        TupleSide side;
        size_t offset = 0;
        const char *value = nullptr;
    };

    struct BoundCondition {
        BoundOperand lhs;
        BoundOperand rhs;
        ColType type;
        int len;
        CompOp op;
    };

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t left_len_;
    size_t right_len_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    std::vector<BoundCondition> bound_conds_;

    size_t join_buffer_size_;
    size_t right_block_capacity_;
    std::unique_ptr<char[]> right_block_;
    size_t right_block_count_ = 0;
    bool right_exhausted_ = false;

    bool is_end_ = false;
    bool left_scan_had_tuple_ = false;
    size_t right_block_pos_ = 0;
    std::unique_ptr<RmRecord> current_left_;
    std::unique_ptr<RmRecord> current_tuple_;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds, size_t join_buffer_size = DEFAULT_JOIN_BUFFER_SIZE)
        : left_(std::move(left)),
          right_(std::move(right)),
          left_len_(left_->tupleLen()),
          right_len_(right_->tupleLen()),
          len_(left_len_ + right_len_),
          fed_conds_(std::move(conds)),
          join_buffer_size_(
              std::min(DEFAULT_JOIN_BUFFER_SIZE,
                       std::max(join_buffer_size, std::max<size_t>(1, right_len_)))),
          right_block_capacity_(std::max<size_t>(1, join_buffer_size_ / std::max<size_t>(1, right_len_))) {
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_len_;
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());

        current_tuple_ = std::make_unique<RmRecord>(len_);
        bind_conditions();
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return is_end_; }

    size_t joinBufferSize() const { return join_buffer_size_; }

    size_t rightBlockCapacity() const { return right_block_capacity_; }

    bool isJoinBufferAllocated() const { return right_block_ != nullptr; }

    void beginTuple() override {
        if (right_block_ == nullptr) {
            right_block_ = std::make_unique<char[]>(right_block_capacity_ * right_len_);
        }
        right_block_count_ = 0;
        right_block_pos_ = 0;
        right_exhausted_ = false;
        current_left_.reset();
        left_scan_had_tuple_ = false;
        is_end_ = false;

        right_->beginTuple();
        if (!load_right_block()) {
            is_end_ = true;
            return;
        }

        left_->beginTuple();
        fetch_next_joined_tuple();
    }

    void nextTuple() override {
        if (!is_end_) {
            fetch_next_joined_tuple();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_tuple_);
    }

    Rid &rid() override { return _abstract_rid; }

   private:
    BoundOperand bind_column(const TabCol &target) const {
        auto find_col = [&](const std::vector<ColMeta> &cols) {
            return std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
                return col.tab_name == target.tab_name && col.name == target.col_name;
            });
        };

        const auto &left_cols = left_->cols();
        auto left_col = find_col(left_cols);
        if (left_col != left_cols.end()) {
            return {TupleSide::LEFT, static_cast<size_t>(left_col->offset), nullptr};
        }

        const auto &right_cols = right_->cols();
        auto right_col = find_col(right_cols);
        if (right_col != right_cols.end()) {
            return {TupleSide::RIGHT, static_cast<size_t>(right_col->offset), nullptr};
        }
        throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
    }

    void bind_conditions() {
        bound_conds_.reserve(fed_conds_.size());
        for (const auto &cond : fed_conds_) {
            auto lhs_col = get_col(cols_, cond.lhs_col);
            BoundCondition bound{
                .lhs = bind_column(cond.lhs_col),
                .rhs = cond.is_rhs_val
                           ? BoundOperand{TupleSide::VALUE, 0, cond.rhs_val.raw->data}
                           : bind_column(cond.rhs_col),
                .type = lhs_col->type,
                .len = lhs_col->len,
                .op = cond.op,
            };
            bound_conds_.push_back(bound);
        }
    }

    const char *operand_data(const BoundOperand &operand, const RmRecord *left_rec,
                             const char *right_rec) const {
        switch (operand.side) {
            case TupleSide::LEFT:
                return left_rec->data + operand.offset;
            case TupleSide::RIGHT:
                return right_rec + operand.offset;
            case TupleSide::VALUE:
                return operand.value;
        }
        return nullptr;
    }

    bool eval_join_conditions(const RmRecord *left_rec, const char *right_rec) {
        for (const auto &cond : bound_conds_) {
            const char *lhs = operand_data(cond.lhs, left_rec, right_rec);
            const char *rhs = operand_data(cond.rhs, left_rec, right_rec);
            int cmp = compare(cond.type, cond.len, lhs, rhs);
            bool matched = false;
            switch (cond.op) {
                case OP_EQ:
                    matched = cmp == 0;
                    break;
                case OP_NE:
                    matched = cmp != 0;
                    break;
                case OP_LT:
                    matched = cmp < 0;
                    break;
                case OP_GT:
                    matched = cmp > 0;
                    break;
                case OP_LE:
                    matched = cmp <= 0;
                    break;
                case OP_GE:
                    matched = cmp >= 0;
                    break;
            }
            if (!matched) {
                return false;
            }
        }
        return true;
    }

    bool load_right_block() {
        right_block_count_ = 0;
        right_block_pos_ = 0;

        while (!right_->is_end() && right_block_count_ < right_block_capacity_) {
            auto rec = right_->Next();
            if (rec != nullptr) {
                memcpy(right_block_.get() + right_block_count_ * right_len_, rec->data, right_len_);
                ++right_block_count_;
            }
            right_->nextTuple();
        }
        right_exhausted_ = right_->is_end();
        return right_block_count_ > 0;
    }

    bool load_current_left() {
        current_left_.reset();
        while (!left_->is_end()) {
            auto rec = left_->Next();
            if (rec != nullptr) {
                current_left_ = std::move(rec);
                left_scan_had_tuple_ = true;
                right_block_pos_ = 0;
                return true;
            }
            left_->nextTuple();
        }
        return false;
    }

    bool advance_to_next_right_block() {
        if (right_exhausted_ || !load_right_block()) {
            return false;
        }
        current_left_.reset();
        left_scan_had_tuple_ = false;
        left_->beginTuple();
        return true;
    }

    void write_current_tuple(const RmRecord *left_rec, const char *right_rec) {
        memcpy(current_tuple_->data, left_rec->data, left_len_);
        memcpy(current_tuple_->data + left_len_, right_rec, right_len_);
    }

    void fetch_next_joined_tuple() {
        while (true) {
            if (current_left_ == nullptr && !load_current_left()) {
                if (!left_scan_had_tuple_) {
                    is_end_ = true;
                    return;
                }
                if (!advance_to_next_right_block()) {
                    is_end_ = true;
                    return;
                }
                continue;
            }

            while (right_block_pos_ < right_block_count_) {
                const char *right_rec = right_block_.get() + right_block_pos_ * right_len_;
                ++right_block_pos_;
                if (eval_join_conditions(current_left_.get(), right_rec)) {
                    write_current_tuple(current_left_.get(), right_rec);
                    return;
                }
            }

            current_left_.reset();
            if (!left_->is_end()) {
                left_->nextTuple();
            }
        }
    }
};
