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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

#include "executor_abstract.h"
#include "executor_nestedloop_join.h"

class HashJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t DEFAULT_HASH_MEMORY_SIZE = 64 * 1024 * 1024;
    static constexpr int32_t EMPTY_SLOT = -1;

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

    struct HashKey {
        size_t left_offset;
        size_t right_offset;
        ColType type;
        int len;
    };

    struct HashSlot {
        uint64_t hash;
        int32_t record_index;
        int32_t reserved;
    };

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    std::unique_ptr<NestedLoopJoinExecutor> fallback_;
    std::vector<Condition> conditions_;
    std::vector<BoundCondition> bound_conditions_;
    std::vector<HashKey> hash_keys_;

    size_t left_len_;
    size_t right_len_;
    size_t len_;
    std::vector<ColMeta> cols_;
    bool build_left_;
    size_t memory_limit_;

    std::unique_ptr<char[]> hash_memory_;
    HashSlot *slots_ = nullptr;
    char *records_ = nullptr;
    size_t slot_capacity_ = 0;
    size_t record_capacity_ = 0;
    size_t record_count_ = 0;
    size_t build_len_ = 0;

    std::unique_ptr<RmRecord> current_probe_;
    std::unique_ptr<RmRecord> current_tuple_;
    uint64_t probe_hash_ = 0;
    size_t probe_slot_ = 0;
    size_t probe_slots_scanned_ = 0;
    bool probe_ready_ = false;
    bool is_end_ = false;

   public:
    HashJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                     std::vector<Condition> conditions, bool build_left,
                     size_t memory_limit = DEFAULT_HASH_MEMORY_SIZE)
        : left_(std::move(left)),
          right_(std::move(right)),
          conditions_(std::move(conditions)),
          left_len_(left_->tupleLen()),
          right_len_(right_->tupleLen()),
          len_(left_len_ + right_len_),
          build_left_(build_left),
          memory_limit_(std::min(DEFAULT_HASH_MEMORY_SIZE, memory_limit)),
          build_len_(build_left_ ? left_len_ : right_len_) {
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_len_;
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        current_tuple_ = std::make_unique<RmRecord>(len_);
        bind_conditions_and_keys();
        if (hash_keys_.empty()) {
            throw InternalError("Hash join requires an equality condition across both inputs");
        }
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return fallback_ != nullptr ? fallback_->is_end() : is_end_; }

    bool usingFallback() const { return fallback_ != nullptr; }

    size_t memoryLimit() const { return memory_limit_; }

    void beginTuple() override {
        if (fallback_ != nullptr) {
            fallback_->beginTuple();
            return;
        }
        reset_probe();
        is_end_ = false;
        if (!prepare_hash_storage() || !build_hash_table()) {
            activate_fallback();
            return;
        }
        if (record_count_ == 0) {
            is_end_ = true;
            return;
        }
        probe_executor()->beginTuple();
        fetch_next_joined_tuple();
    }

    void nextTuple() override {
        if (fallback_ != nullptr) {
            fallback_->nextTuple();
        } else if (!is_end_) {
            fetch_next_joined_tuple();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (fallback_ != nullptr) {
            return fallback_->Next();
        }
        if (is_end_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_tuple_);
    }

    Rid &rid() override { return fallback_ != nullptr ? fallback_->rid() : _abstract_rid; }

   private:
    AbstractExecutor *build_executor() { return build_left_ ? left_.get() : right_.get(); }

    AbstractExecutor *probe_executor() { return build_left_ ? right_.get() : left_.get(); }

    static bool find_column(const std::vector<ColMeta> &cols, const TabCol &target, size_t *offset,
                            ColType *type = nullptr, int *len = nullptr) {
        auto column = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &meta) {
            return meta.tab_name == target.tab_name && meta.name == target.col_name;
        });
        if (column == cols.end()) {
            return false;
        }
        *offset = static_cast<size_t>(column->offset);
        if (type != nullptr) {
            *type = column->type;
        }
        if (len != nullptr) {
            *len = column->len;
        }
        return true;
    }

    BoundOperand bind_operand(const TabCol &column) const {
        size_t offset = 0;
        if (find_column(left_->cols(), column, &offset)) {
            return {TupleSide::LEFT, offset, nullptr};
        }
        if (find_column(right_->cols(), column, &offset)) {
            return {TupleSide::RIGHT, offset, nullptr};
        }
        throw ColumnNotFoundError(column.tab_name + '.' + column.col_name);
    }

    void bind_conditions_and_keys() {
        for (const auto &condition : conditions_) {
            size_t lhs_offset = 0;
            ColType lhs_type;
            int lhs_len = 0;
            bool lhs_left = find_column(left_->cols(), condition.lhs_col, &lhs_offset, &lhs_type, &lhs_len);
            bool lhs_right =
                !lhs_left && find_column(right_->cols(), condition.lhs_col, &lhs_offset, &lhs_type, &lhs_len);
            if (!lhs_left && !lhs_right) {
                throw ColumnNotFoundError(condition.lhs_col.tab_name + '.' + condition.lhs_col.col_name);
            }

            BoundCondition bound{
                .lhs = bind_operand(condition.lhs_col),
                .rhs = condition.is_rhs_val
                           ? BoundOperand{TupleSide::VALUE, 0, condition.rhs_val.raw->data}
                           : bind_operand(condition.rhs_col),
                .type = lhs_type,
                .len = lhs_len,
                .op = condition.op,
            };
            bound_conditions_.push_back(bound);

            if (condition.is_rhs_val || condition.op != OP_EQ) {
                continue;
            }
            size_t rhs_offset = 0;
            bool rhs_left = find_column(left_->cols(), condition.rhs_col, &rhs_offset);
            bool rhs_right = !rhs_left && find_column(right_->cols(), condition.rhs_col, &rhs_offset);
            if (lhs_left && rhs_right) {
                hash_keys_.push_back({lhs_offset, rhs_offset, lhs_type, lhs_len});
            } else if (lhs_right && rhs_left) {
                hash_keys_.push_back({rhs_offset, lhs_offset, lhs_type, lhs_len});
            }
        }
    }

    bool prepare_hash_storage() {
        slot_capacity_ = 0;
        record_capacity_ = 0;
        if (memory_limit_ < sizeof(HashSlot) * 2 + build_len_) {
            return false;
        }
        for (size_t slots = 2;; slots *= 2) {
            if (slots > memory_limit_ / sizeof(HashSlot)) {
                break;
            }
            size_t slot_bytes = slots * sizeof(HashSlot);
            size_t records = std::min(slots / 2, (memory_limit_ - slot_bytes) / build_len_);
            if (records > record_capacity_) {
                slot_capacity_ = slots;
                record_capacity_ = records;
            }
            if (slots > std::numeric_limits<size_t>::max() / 2) {
                break;
            }
        }
        if (slot_capacity_ == 0 || record_capacity_ == 0) {
            return false;
        }

        hash_memory_ = std::make_unique<char[]>(memory_limit_);
        slots_ = reinterpret_cast<HashSlot *>(hash_memory_.get());
        records_ = hash_memory_.get() + slot_capacity_ * sizeof(HashSlot);
        for (size_t i = 0; i < slot_capacity_; ++i) {
            new (slots_ + i) HashSlot{0, EMPTY_SLOT, 0};
        }
        record_count_ = 0;
        return true;
    }

    static uint64_t hash_bytes(uint64_t hash, const char *data, size_t len) {
        constexpr uint64_t FNV_PRIME = 1099511628211ULL;
        for (size_t i = 0; i < len; ++i) {
            hash ^= static_cast<unsigned char>(data[i]);
            hash *= FNV_PRIME;
        }
        return hash;
    }

    static uint64_t hash_value(uint64_t hash, ColType type, const char *data, int len) {
        if (type == TYPE_FLOAT) {
            float value;
            memcpy(&value, data, sizeof(value));
            uint32_t bits = 0;
            if (value == 0.0F) {
                bits = 0;
            } else if (std::isnan(value)) {
                bits = 0x7fc00000U;
            } else {
                memcpy(&bits, &value, sizeof(bits));
            }
            return hash_bytes(hash, reinterpret_cast<const char *>(&bits), sizeof(bits));
        }
        return hash_bytes(hash, data, len);
    }

    uint64_t tuple_hash(const char *tuple, bool left_side) const {
        uint64_t hash = 1469598103934665603ULL;
        for (const auto &key : hash_keys_) {
            size_t offset = left_side ? key.left_offset : key.right_offset;
            hash = hash_value(hash, key.type, tuple + offset, key.len);
        }
        return hash;
    }

    bool insert_build_record(const RmRecord &record) {
        if (record_count_ >= record_capacity_) {
            return false;
        }
        uint64_t hash = tuple_hash(record.data, build_left_);
        size_t slot = hash & (slot_capacity_ - 1);
        while (slots_[slot].record_index != EMPTY_SLOT) {
            slot = (slot + 1) & (slot_capacity_ - 1);
        }
        memcpy(records_ + record_count_ * build_len_, record.data, build_len_);
        slots_[slot].hash = hash;
        slots_[slot].record_index = static_cast<int32_t>(record_count_);
        ++record_count_;
        return true;
    }

    bool build_hash_table() {
        auto *executor = build_executor();
        for (executor->beginTuple(); !executor->is_end(); executor->nextTuple()) {
            auto record = executor->Next();
            if (record != nullptr && !insert_build_record(*record)) {
                return false;
            }
        }
        return true;
    }

    void activate_fallback() {
        hash_memory_.reset();
        slots_ = nullptr;
        records_ = nullptr;
        fallback_ = std::make_unique<NestedLoopJoinExecutor>(
            std::move(left_), std::move(right_), conditions_);
        fallback_->beginTuple();
    }

    void reset_probe() {
        current_probe_.reset();
        probe_hash_ = 0;
        probe_slot_ = 0;
        probe_slots_scanned_ = 0;
        probe_ready_ = false;
    }

    const char *operand_data(const BoundOperand &operand, const char *left, const char *right) const {
        switch (operand.side) {
            case TupleSide::LEFT:
                return left + operand.offset;
            case TupleSide::RIGHT:
                return right + operand.offset;
            case TupleSide::VALUE:
                return operand.value;
        }
        return nullptr;
    }

    bool eval_conditions(const char *left, const char *right) {
        for (const auto &condition : bound_conditions_) {
            int cmp = compare(condition.type, condition.len,
                              operand_data(condition.lhs, left, right),
                              operand_data(condition.rhs, left, right));
            bool matched = false;
            switch (condition.op) {
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

    void write_current_tuple(const char *left, const char *right) {
        memcpy(current_tuple_->data, left, left_len_);
        memcpy(current_tuple_->data + left_len_, right, right_len_);
    }

    void start_probe_record(std::unique_ptr<RmRecord> record) {
        current_probe_ = std::move(record);
        probe_hash_ = tuple_hash(current_probe_->data, !build_left_);
        probe_slot_ = probe_hash_ & (slot_capacity_ - 1);
        probe_slots_scanned_ = 0;
        probe_ready_ = true;
    }

    void fetch_next_joined_tuple() {
        auto *executor = probe_executor();
        while (true) {
            if (!probe_ready_) {
                while (!executor->is_end()) {
                    auto record = executor->Next();
                    executor->nextTuple();
                    if (record != nullptr) {
                        start_probe_record(std::move(record));
                        break;
                    }
                }
                if (!probe_ready_) {
                    is_end_ = true;
                    return;
                }
            }

            while (probe_slots_scanned_ < slot_capacity_) {
                const auto &slot = slots_[probe_slot_];
                probe_slot_ = (probe_slot_ + 1) & (slot_capacity_ - 1);
                ++probe_slots_scanned_;
                if (slot.record_index == EMPTY_SLOT) {
                    break;
                }
                if (slot.hash != probe_hash_) {
                    continue;
                }
                const char *build_record = records_ + static_cast<size_t>(slot.record_index) * build_len_;
                const char *left = build_left_ ? build_record : current_probe_->data;
                const char *right = build_left_ ? current_probe_->data : build_record;
                if (eval_conditions(left, right)) {
                    write_current_tuple(left, right);
                    return;
                }
            }
            current_probe_.reset();
            probe_ready_ = false;
        }
    }
};
