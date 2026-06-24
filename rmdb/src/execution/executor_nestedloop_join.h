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
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t DEFAULT_JOIN_BUFFER_SIZE = 8 * 1024 * 1024;

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    size_t join_buffer_size_;
    size_t left_block_capacity_;

    bool is_end_ = false;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    size_t left_block_pos_ = 0;
    std::unique_ptr<RmRecord> current_right_;
    std::unique_ptr<RmRecord> current_tuple_;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds, size_t join_buffer_size = DEFAULT_JOIN_BUFFER_SIZE)
        : left_(std::move(left)),
          right_(std::move(right)),
          len_(left_->tupleLen() + right_->tupleLen()),
          fed_conds_(std::move(conds)),
          join_buffer_size_(std::max<size_t>(join_buffer_size, left_->tupleLen())) {
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        left_block_capacity_ = std::max<size_t>(1, join_buffer_size_ / std::max<size_t>(1, left_->tupleLen()));
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return is_end_; }

    void beginTuple() override {
        left_->beginTuple();
        left_block_.clear();
        left_block_pos_ = 0;
        current_right_.reset();
        current_tuple_.reset();
        is_end_ = false;

        if (!load_left_block()) {
            is_end_ = true;
            return;
        }
        right_->beginTuple();
        fetch_next_joined_tuple();
    }

    void nextTuple() override {
        if (is_end_) {
            return;
        }
        fetch_next_joined_tuple();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_ || current_tuple_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_tuple_);
    }

    Rid &rid() override { return _abstract_rid; }

   private:
    bool load_left_block() {
        left_block_.clear();
        left_block_pos_ = 0;
        while (!left_->is_end() && left_block_.size() < left_block_capacity_) {
            auto rec = left_->Next();
            if (rec != nullptr) {
                left_block_.push_back(std::move(rec));
            }
            left_->nextTuple();
        }
        return !left_block_.empty();
    }

    bool load_current_right_tuple() {
        while (!right_->is_end()) {
            auto rec = right_->Next();
            if (rec != nullptr) {
                current_right_ = std::move(rec);
                return true;
            }
            right_->nextTuple();
        }
        return false;
    }

    void advance_right_tuple() {
        current_right_.reset();
        if (!right_->is_end()) {
            right_->nextTuple();
        }
        left_block_pos_ = 0;
    }

    std::unique_ptr<RmRecord> make_joined_record(const RmRecord *left_rec, const RmRecord *right_rec) const {
        auto joined = std::make_unique<RmRecord>(len_);
        memcpy(joined->data, left_rec->data, left_->tupleLen());
        memcpy(joined->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return joined;
    }

    void fetch_next_joined_tuple() {
        current_tuple_.reset();
        while (true) {
            if (left_block_.empty()) {
                is_end_ = true;
                return;
            }

            if (current_right_ == nullptr && !load_current_right_tuple()) {
                if (!load_left_block()) {
                    is_end_ = true;
                    return;
                }
                right_->beginTuple();
                current_right_.reset();
                continue;
            }

            while (current_right_ != nullptr && left_block_pos_ < left_block_.size()) {
                auto joined = make_joined_record(left_block_[left_block_pos_].get(), current_right_.get());
                ++left_block_pos_;
                if (eval_conds(cols_, joined.get(), fed_conds_)) {
                    current_tuple_ = std::move(joined);
                    return;
                }
            }

            if (current_right_ != nullptr && left_block_pos_ >= left_block_.size()) {
                advance_right_tuple();
            }
        }
    }
};
