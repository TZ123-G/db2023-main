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

    const bool buffer_left_;

    // Block buffer for whichever side is buffered (flat char array)
    size_t block_rec_len_;
    size_t block_capacity_;
    std::unique_ptr<char[]> block_;
    size_t block_count_ = 0;
    size_t block_pos_ = 0;
    bool block_exhausted_ = false;

    // Outer-side state (the side being scanned per block)
    std::unique_ptr<RmRecord> current_outer_;
    bool outer_scan_had_tuple_ = false;

    bool is_end_ = false;
    std::unique_ptr<RmRecord> current_tuple_;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds, bool buffer_left = false,
                           size_t join_buffer_size = DEFAULT_JOIN_BUFFER_SIZE)
        : left_(std::move(left)),
          right_(std::move(right)),
          left_len_(left_->tupleLen()),
          right_len_(right_->tupleLen()),
          len_(left_len_ + right_len_),
          fed_conds_(std::move(conds)),
          buffer_left_(buffer_left),
          block_rec_len_(buffer_left ? left_len_ : right_len_),
          block_capacity_(std::max<size_t>(1,
              std::min(DEFAULT_JOIN_BUFFER_SIZE, std::max(join_buffer_size, block_rec_len_)) / block_rec_len_)) {
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

    size_t joinBufferSize() const {
        return block_capacity_ * block_rec_len_;
    }

    size_t blockCapacity() const { return block_capacity_; }

    bool isBlockAllocated() const { return block_ != nullptr; }

    void beginTuple() override {
        if (block_ == nullptr) {
            block_ = std::make_unique<char[]>(block_capacity_ * block_rec_len_);
        }
        block_count_ = 0;
        block_pos_ = 0;
        block_exhausted_ = false;
        current_outer_.reset();
        outer_scan_had_tuple_ = false;
        is_end_ = false;

        // Always buffer the designated side first, then start the outer scan
        auto &buffer_src = buffer_left_ ? left_ : right_;
        auto &outer_src  = buffer_left_ ? right_ : left_;

        buffer_src->beginTuple();
        if (!load_block()) {
            is_end_ = true;
            return;
        }

        outer_src->beginTuple();
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
    // ----- Condition binding (unchanged) -----

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

    // ----- Evaluation (raw-data version, direction-agnostic) -----

    static const char *resolve_operand(const BoundOperand &operand,
                                        const char *left_data, const char *right_data) {
        switch (operand.side) {
            case TupleSide::LEFT:  return left_data + operand.offset;
            case TupleSide::RIGHT: return right_data + operand.offset;
            case TupleSide::VALUE: return operand.value;
        }
        return nullptr;
    }

    bool eval_conds_raw(const char *left_data, const char *right_data) {
        for (const auto &cond : bound_conds_) {
            const char *lhs = resolve_operand(cond.lhs, left_data, right_data);
            const char *rhs = resolve_operand(cond.rhs, left_data, right_data);
            int cmp = compare(cond.type, cond.len, lhs, rhs);
            switch (cond.op) {
                case OP_EQ: if (cmp != 0) return false; break;
                case OP_NE: if (cmp == 0) return false; break;
                case OP_LT: if (cmp >= 0) return false; break;
                case OP_GT: if (cmp <= 0) return false; break;
                case OP_LE: if (cmp > 0) return false; break;
                case OP_GE: if (cmp < 0) return false; break;
            }
        }
        return true;
    }

    // ----- Direction-agnostic block/outer helpers -----

    /** Source executor for the side being buffered into block_ */
    std::unique_ptr<AbstractExecutor> &buffer_src() {
        return buffer_left_ ? left_ : right_;
    }

    /** Source executor for the outer (scanned-per-block) side */
    std::unique_ptr<AbstractExecutor> &outer_src() {
        return buffer_left_ ? right_ : left_;
    }

    bool load_block() {
        block_count_ = 0;
        block_pos_ = 0;
        auto &src = buffer_src();
        while (!src->is_end() && block_count_ < block_capacity_) {
            auto rec = src->Next();
            if (rec != nullptr) {
                memcpy(block_.get() + block_count_ * block_rec_len_, rec->data, block_rec_len_);
                ++block_count_;
            }
            src->nextTuple();
        }
        block_exhausted_ = src->is_end();
        return block_count_ > 0;
    }

    bool load_current_outer() {
        current_outer_.reset();
        auto &src = outer_src();
        while (!src->is_end()) {
            auto rec = src->Next();
            if (rec != nullptr) {
                current_outer_ = std::move(rec);
                outer_scan_had_tuple_ = true;
                block_pos_ = 0;
                return true;
            }
            src->nextTuple();
        }
        return false;
    }

    bool advance_to_next_block() {
        if (block_exhausted_ || !load_block()) {
            return false;
        }
        current_outer_.reset();
        outer_scan_had_tuple_ = false;
        outer_src()->beginTuple();
        return true;
    }

    void write_joined_tuple(const char *left_data, const char *right_data) {
        memcpy(current_tuple_->data, left_data, left_len_);
        memcpy(current_tuple_->data + left_len_, right_data, right_len_);
    }

    void fetch_next_joined_tuple() {
        while (true) {
            if (current_outer_ == nullptr && !load_current_outer()) {
                if (!outer_scan_had_tuple_) {
                    is_end_ = true;
                    return;
                }
                if (!advance_to_next_block()) {
                    is_end_ = true;
                    return;
                }
                continue;
            }

            const char *outer_data = current_outer_->data;

            while (block_pos_ < block_count_) {
                const char *block_rec = block_.get() + block_pos_ * block_rec_len_;
                ++block_pos_;

                bool matched;
                if (buffer_left_) {
                    // Block is LEFT, outer is RIGHT
                    matched = eval_conds_raw(block_rec, outer_data);
                } else {
                    // Block is RIGHT, outer is LEFT
                    matched = eval_conds_raw(outer_data, block_rec);
                }

                if (matched) {
                    if (buffer_left_) {
                        write_joined_tuple(block_rec, outer_data);
                    } else {
                        write_joined_tuple(outer_data, block_rec);
                    }
                    return;
                }
            }

            // Block exhausted for current outer tuple — advance outer
            current_outer_.reset();
            auto &src = outer_src();
            if (!src->is_end()) {
                src->nextTuple();
            }
        }
    }
};
