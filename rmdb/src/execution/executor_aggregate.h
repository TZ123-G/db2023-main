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
#include <memory>
#include <vector>

#include "executor_abstract.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<AggregateExpr> aggregates_;
    std::vector<ColMeta> cols_;
    std::vector<size_t> source_col_idxs_;
    size_t len_;
    bool is_end_;
    std::unique_ptr<RmRecord> result_;

    template <typename T>
    static T read_value(const char *data) {
        T value;
        memcpy(&value, data, sizeof(T));
        return value;
    }

    template <typename T>
    static void write_value(char *data, T value) {
        memcpy(data, &value, sizeof(T));
    }

    static int compare_value(ColType type, int len, const char *lhs, const char *rhs) {
        if (type == TYPE_INT) {
            int lhs_value = read_value<int>(lhs);
            int rhs_value = read_value<int>(rhs);
            return (lhs_value > rhs_value) - (lhs_value < rhs_value);
        }
        if (type == TYPE_FLOAT) {
            float lhs_value = read_value<float>(lhs);
            float rhs_value = read_value<float>(rhs);
            return (lhs_value > rhs_value) - (lhs_value < rhs_value);
        }
        return memcmp(lhs, rhs, len);
    }

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<AggregateExpr> aggregates)
        : prev_(std::move(prev)), aggregates_(std::move(aggregates)), len_(0), is_end_(true) {
        const auto &prev_cols = prev_->cols();
        for (const auto &aggregate : aggregates_) {
            ColMeta result_col;
            result_col.tab_name = "";
            result_col.name = aggregate.output_name;
            result_col.offset = len_;
            result_col.index = false;

            if (aggregate.type == AGG_COUNT) {
                result_col.type = TYPE_INT;
                result_col.len = sizeof(int);
                source_col_idxs_.push_back(std::numeric_limits<size_t>::max());
            } else {
                auto source_col = get_col(prev_cols, aggregate.col);
                result_col.type = source_col->type;
                result_col.len = source_col->len;
                source_col_idxs_.push_back(source_col - prev_cols.begin());
            }
            len_ += result_col.len;
            cols_.push_back(std::move(result_col));
        }
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override {
        result_ = std::make_unique<RmRecord>(len_);
        memset(result_->data, 0, len_);
        std::vector<bool> initialized(aggregates_.size(), false);
        const auto &prev_cols = prev_->cols();
        bool has_rows = false;

        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto record = prev_->Next();
            if (record == nullptr) {
                continue;
            }
            has_rows = true;
            for (size_t i = 0; i < aggregates_.size(); ++i) {
                const auto &aggregate = aggregates_[i];
                char *destination = result_->data + cols_[i].offset;
                if (aggregate.type == AGG_COUNT) {
                    write_value<int>(destination, read_value<int>(destination) + 1);
                    continue;
                }

                const auto &source_col = prev_cols[source_col_idxs_[i]];
                const char *source = record->data + source_col.offset;
                if (aggregate.type == AGG_SUM) {
                    if (source_col.type == TYPE_INT) {
                        write_value<int>(destination,
                                         read_value<int>(destination) + read_value<int>(source));
                    } else {
                        write_value<float>(destination,
                                           read_value<float>(destination) + read_value<float>(source));
                    }
                    continue;
                }

                if (!initialized[i]) {
                    memcpy(destination, source, source_col.len);
                    initialized[i] = true;
                    continue;
                }
                int cmp = compare_value(source_col.type, source_col.len, source, destination);
                if ((aggregate.type == AGG_MAX && cmp > 0) || (aggregate.type == AGG_MIN && cmp < 0)) {
                    memcpy(destination, source, source_col.len);
                }
            }
        }
        is_end_ = !has_rows;
    }

    void nextTuple() override { is_end_ = true; }

    bool is_end() const override { return is_end_; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_ || result_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*result_);
    }

    Rid &rid() override { return _abstract_rid; }
};
