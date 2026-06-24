/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#undef NDEBUG

#define private public

#include "record/rm.h"
#include "storage/buffer_pool_manager.h"

#undef private

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>  // NOLINT
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "common/datetime.h"
#include "execution/executor_hash_join.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/execution_sort.h"
#include "index/ix_index_handle.h"
#include "optimizer/predicate_inference.h"
#include "replacer/lru_replacer.h"
#include "storage/disk_manager.h"

const std::string TEST_DB_NAME = "BufferPoolManagerTest_db";  // 以数据库名作为根目录
const std::string TEST_FILE_NAME = "basic";                   // 测试文件的名字
const std::string TEST_FILE_NAME_CCUR = "concurrency";        // 测试文件的名字
const std::string TEST_FILE_NAME_BIG = "bigdata";             // 测试文件的名字
constexpr int MAX_FILES = 32;
constexpr int MAX_PAGES = 128;
constexpr size_t TEST_BUFFER_POOL_SIZE = MAX_FILES * MAX_PAGES;

TEST(DatetimeTest, ParseAndFormatValidValues) {
    std::vector<std::string> values = {
        "1000-01-01 00:00:00",
        "1999-07-07 12:30:00",
        "2000-02-29 23:59:59",
        "2024-02-29 09:12:19",
        "9999-12-31 23:59:59",
    };
    for (const auto &value : values) {
        EXPECT_EQ(format_datetime(parse_datetime(value)), value);
    }
}

TEST(DatetimeTest, RejectInvalidValues) {
    std::vector<std::string> values = {
        "0999-12-31 23:59:59",
        "10000-01-01 00:00:00",
        "1999-13-07 12:30:00",
        "1999-1-07 12:30:00",
        "1999-00-07 12:30:00",
        "1999-07-00 12:30:00",
        "1999-04-31 12:30:00",
        "1999-02-29 12:30:00",
        "1900-02-29 12:30:00",
        "2100-02-29 12:30:00",
        "1999-02-30 12:30:00",
        "1999-02-28 24:00:00",
        "1999-02-28 12:60:00",
        "1999-02-28 12:30:60",
        "-999-07-07 12:30:00",
        "1999/07/07 12:30:00",
        "1999-07-07T12:30:00",
        "1999-07-07 12-30-00",
        "1999-07-0a 12:30:00",
        " 1999-07-07 12:30:00",
        "1999-07-07 12:30:00 ",
    };
    for (const auto &value : values) {
        EXPECT_THROW(parse_datetime(value), RMDBError) << value;
    }
}

TEST(DatetimeTest, EncodedValuesPreserveChronologicalOrder) {
    int64_t earlier = parse_datetime("2023-05-18 09:12:19");
    int64_t later = parse_datetime("2023-05-30 12:34:32");
    EXPECT_LT(earlier, later);
    EXPECT_LT(parse_datetime("2023-12-31 23:59:59"), parse_datetime("2024-01-01 00:00:00"));
    EXPECT_LT(ix_compare(reinterpret_cast<const char *>(&earlier), reinterpret_cast<const char *>(&later),
                         TYPE_DATETIME, DATETIME_LEN),
              0);
}

TEST(BigintTest, RawEncodingAndComparison) {
    Value value;
    value.set_bigint(-922337203685477580LL);
    value.init_raw(sizeof(int64_t));

    int64_t decoded = 0;
    memcpy(&decoded, value.raw->data, sizeof(decoded));
    EXPECT_EQ(decoded, -922337203685477580LL);

    int64_t smaller = -922337203685477580LL;
    int64_t larger = 372036854775807LL;
    EXPECT_LT(ix_compare(reinterpret_cast<const char *>(&smaller), reinterpret_cast<const char *>(&larger),
                         TYPE_BIGINT, sizeof(int64_t)),
              0);
}

TEST(IndexNodeTest, MaintainsSortedUniqueKeysAndBounds) {
    IxFileHdr file_hdr;
    file_hdr.col_num_ = 1;
    file_hdr.col_types_ = {TYPE_INT};
    file_hdr.col_lens_ = {static_cast<int>(sizeof(int))};
    file_hdr.col_tot_len_ = sizeof(int);
    file_hdr.btree_order_ = 8;
    file_hdr.keys_size_ = (file_hdr.btree_order_ + 1) * file_hdr.col_tot_len_;

    Page page;
    auto page_hdr = reinterpret_cast<IxPageHdr *>(page.get_data());
    page_hdr->num_key = 0;
    page_hdr->is_leaf = true;
    IxNodeHandle node(&file_hdr, &page);

    for (int key : {5, 1, 9, 3, 7}) {
        node.insert(reinterpret_cast<const char *>(&key), Rid{key, key});
    }
    int duplicate = 5;
    node.insert(reinterpret_cast<const char *>(&duplicate), Rid{99, 99});
    EXPECT_EQ(node.get_size(), 5);

    int four = 4;
    int five = 5;
    EXPECT_EQ(node.lower_bound(reinterpret_cast<const char *>(&four)), 2);
    EXPECT_EQ(node.lower_bound(reinterpret_cast<const char *>(&five)), 2);
    EXPECT_EQ(node.upper_bound(reinterpret_cast<const char *>(&five)), 3);
    EXPECT_EQ(node.get_rid(2)->page_no, 5);

    node.remove(reinterpret_cast<const char *>(&five));
    EXPECT_EQ(node.get_size(), 4);
    EXPECT_EQ(node.lower_bound(reinterpret_cast<const char *>(&five)), 2);
    EXPECT_EQ(node.get_rid(2)->page_no, 7);
}

struct MockTupleExecutorStats {
    size_t begin_count = 0;
    size_t next_count = 0;
};

class MockTupleExecutor : public AbstractExecutor {
   public:
    MockTupleExecutor(std::vector<ColMeta> cols, std::vector<RmRecord> rows,
                      std::shared_ptr<MockTupleExecutorStats> stats = nullptr)
        : cols_(std::move(cols)),
          rows_(std::move(rows)),
          tuple_len_(cols_.empty() ? 0 : cols_.back().offset + cols_.back().len),
          stats_(std::move(stats)) {}

    size_t tupleLen() const override { return tuple_len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override {
        if (stats_ != nullptr) {
            ++stats_->begin_count;
        }
        cursor_ = 0;
        if (!is_end()) {
            rid_ = {0, static_cast<int>(cursor_)};
        }
    }

    void nextTuple() override {
        if (is_end()) {
            return;
        }
        if (stats_ != nullptr) {
            ++stats_->next_count;
        }
        ++cursor_;
        if (!is_end()) {
            rid_ = {0, static_cast<int>(cursor_)};
        }
    }

    bool is_end() const override { return cursor_ >= rows_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(rows_[cursor_]);
    }

    Rid &rid() override { return rid_; }

   private:
    std::vector<ColMeta> cols_;
    std::vector<RmRecord> rows_;
    size_t tuple_len_;
    std::shared_ptr<MockTupleExecutorStats> stats_;
    size_t cursor_ = 0;
    Rid rid_{};
};

RmRecord make_sort_record(const std::string &company, int order_number) {
    RmRecord record(14);
    memset(record.data, 0, record.size);
    memcpy(record.data, company.c_str(), std::min<size_t>(company.size(), 10));
    memcpy(record.data + 10, &order_number, sizeof(order_number));
    return record;
}

RmRecord make_join_record(int value) {
    RmRecord record(sizeof(int));
    memcpy(record.data, &value, sizeof(value));
    return record;
}

RmRecord make_two_int_record(int first, int second) {
    RmRecord record(sizeof(int) * 2);
    memcpy(record.data, &first, sizeof(first));
    memcpy(record.data + sizeof(int), &second, sizeof(second));
    return record;
}

Condition make_column_condition(const std::string &lhs_table, const std::string &lhs_column, CompOp op,
                                const std::string &rhs_table, const std::string &rhs_column) {
    Condition cond;
    cond.lhs_col = {.tab_name = lhs_table, .col_name = lhs_column};
    cond.op = op;
    cond.is_rhs_val = false;
    cond.rhs_col = {.tab_name = rhs_table, .col_name = rhs_column};
    return cond;
}

Condition make_int_value_condition(const std::string &table, const std::string &column, CompOp op, int value) {
    Condition cond;
    cond.lhs_col = {.tab_name = table, .col_name = column};
    cond.op = op;
    cond.is_rhs_val = true;
    cond.rhs_val.set_int(value);
    cond.rhs_val.init_raw(sizeof(int));
    return cond;
}

std::vector<std::pair<int, int>> collect_join_pairs(AbstractExecutor &executor) {
    std::vector<std::pair<int, int>> pairs;
    for (executor.beginTuple(); !executor.is_end(); executor.nextTuple()) {
        auto record = executor.Next();
        int left_val = 0;
        int right_val = 0;
        memcpy(&left_val, record->data, sizeof(left_val));
        memcpy(&right_val, record->data + sizeof(int), sizeof(right_val));
        pairs.emplace_back(left_val, right_val);
    }
    std::sort(pairs.begin(), pairs.end());
    return pairs;
}

bool has_int_condition(const std::vector<Condition> &conditions, const std::string &table,
                       const std::string &column, CompOp op, int value) {
    return std::any_of(conditions.begin(), conditions.end(), [&](const Condition &condition) {
        if (!condition.is_rhs_val || condition.lhs_col.tab_name != table ||
            condition.lhs_col.col_name != column || condition.op != op ||
            condition.rhs_val.type != TYPE_INT) {
            return false;
        }
        int actual = 0;
        memcpy(&actual, condition.rhs_val.raw->data, sizeof(actual));
        return actual == value;
    });
}

TEST(PredicateInferenceTest, PropagatesBoundsAcrossColumnRelations) {
    std::vector<Condition> conditions = {
        make_column_condition("t1", "a", OP_LT, "t2", "b"),
        make_column_condition("t2", "b", OP_LE, "t3", "c"),
        make_int_value_condition("t3", "c", OP_LT, 1000),
    };

    auto inferred = predicate_inference::infer(conditions);
    EXPECT_TRUE(has_int_condition(inferred, "t2", "b", OP_LT, 1000));
    EXPECT_TRUE(has_int_condition(inferred, "t1", "a", OP_LT, 1000));
}

TEST(PredicateInferenceTest, PropagatesEqualityAndLowerBoundsWithoutDuplicates) {
    std::vector<Condition> conditions = {
        make_column_condition("t1", "a", OP_EQ, "t2", "b"),
        make_column_condition("t2", "b", OP_LT, "t3", "c"),
        make_int_value_condition("t1", "a", OP_GE, 10),
        make_int_value_condition("t1", "a", OP_NE, 20),
    };

    auto inferred = predicate_inference::infer(conditions);
    EXPECT_TRUE(has_int_condition(inferred, "t2", "b", OP_GE, 10));
    EXPECT_TRUE(has_int_condition(inferred, "t3", "c", OP_GT, 10));
    EXPECT_TRUE(has_int_condition(inferred, "t2", "b", OP_NE, 20));

    std::set<std::string> unique;
    for (const auto &condition : inferred) {
        EXPECT_TRUE(unique.insert(predicate_inference::condition_key(condition)).second);
    }
}

TEST(HashJoinExecutorTest, SupportsDuplicateKeysAndResidualConditions) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<RmRecord> left_rows = {
        make_join_record(1), make_join_record(2), make_join_record(2), make_join_record(3),
    };
    std::vector<RmRecord> right_rows = {
        make_join_record(2), make_join_record(2), make_join_record(3), make_join_record(4),
    };
    std::vector<Condition> conditions = {
        make_column_condition("t1", "id", OP_EQ, "t2", "id"),
        make_int_value_condition("t2", "id", OP_LT, 4),
    };

    HashJoinExecutor executor(
        std::make_unique<MockTupleExecutor>(left_cols, left_rows),
        std::make_unique<MockTupleExecutor>(right_cols, right_rows), conditions, true, 4096);
    EXPECT_EQ(collect_join_pairs(executor), (std::vector<std::pair<int, int>>{
        {2, 2}, {2, 2}, {2, 2}, {2, 2}, {3, 3},
    }));
    EXPECT_FALSE(executor.usingFallback());
}

TEST(HashJoinExecutorTest, SupportsCompositeKeysAndEitherBuildSide) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "a", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
        {.tab_name = "t1", .name = "b", .type = TYPE_INT, .len = 4, .offset = 4, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "x", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
        {.tab_name = "t2", .name = "y", .type = TYPE_INT, .len = 4, .offset = 4, .index = false},
    };
    std::vector<RmRecord> left_rows = {
        make_two_int_record(1, 10), make_two_int_record(1, 20), make_two_int_record(2, 10),
    };
    std::vector<RmRecord> right_rows = {
        make_two_int_record(1, 10), make_two_int_record(1, 30), make_two_int_record(2, 10),
    };
    std::vector<Condition> conditions = {
        make_column_condition("t1", "a", OP_EQ, "t2", "x"),
        make_column_condition("t2", "y", OP_EQ, "t1", "b"),
    };

    for (bool build_left : {false, true}) {
        HashJoinExecutor executor(
            std::make_unique<MockTupleExecutor>(left_cols, left_rows),
            std::make_unique<MockTupleExecutor>(right_cols, right_rows), conditions, build_left, 4096);
        for (int run = 0; run < 2; ++run) {
            size_t count = 0;
            for (executor.beginTuple(); !executor.is_end(); executor.nextTuple()) {
                auto record = executor.Next();
                ASSERT_NE(record, nullptr);
                ++count;
            }
            EXPECT_EQ(count, 2);
        }
    }
}

TEST(HashJoinExecutorTest, NormalizesFloatingPointZeroKeys) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "value", .type = TYPE_FLOAT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "value", .type = TYPE_FLOAT, .len = 4, .offset = 0, .index = false},
    };
    auto make_float_record = [](float value) {
        RmRecord record(sizeof(float));
        memcpy(record.data, &value, sizeof(value));
        return record;
    };
    HashJoinExecutor executor(
        std::make_unique<MockTupleExecutor>(
            left_cols, std::vector<RmRecord>{make_float_record(-0.0F)}),
        std::make_unique<MockTupleExecutor>(
            right_cols, std::vector<RmRecord>{make_float_record(0.0F)}),
        std::vector<Condition>{make_column_condition("t1", "value", OP_EQ, "t2", "value")},
        true, 1024);

    executor.beginTuple();
    ASSERT_FALSE(executor.is_end());
    EXPECT_NE(executor.Next(), nullptr);
    executor.nextTuple();
    EXPECT_TRUE(executor.is_end());
}

TEST(HashJoinExecutorTest, SupportsAllHashableColumnTypesInCompositeKeys) {
    constexpr int BIGINT_OFFSET = 0;
    constexpr int FLOAT_OFFSET = BIGINT_OFFSET + sizeof(int64_t);
    constexpr int STRING_OFFSET = FLOAT_OFFSET + sizeof(float);
    constexpr int DATETIME_OFFSET = STRING_OFFSET + 8;
    constexpr int RECORD_SIZE = DATETIME_OFFSET + sizeof(int64_t);
    auto columns = [=](const std::string &table) {
        return std::vector<ColMeta>{
            {.tab_name = table, .name = "big", .type = TYPE_BIGINT, .len = 8,
             .offset = BIGINT_OFFSET, .index = false},
            {.tab_name = table, .name = "ratio", .type = TYPE_FLOAT, .len = 4,
             .offset = FLOAT_OFFSET, .index = false},
            {.tab_name = table, .name = "name", .type = TYPE_STRING, .len = 8,
             .offset = STRING_OFFSET, .index = false},
            {.tab_name = table, .name = "created", .type = TYPE_DATETIME, .len = 8,
             .offset = DATETIME_OFFSET, .index = false},
        };
    };
    auto record = [=](int64_t big, float ratio, const std::string &name, int64_t datetime) {
        RmRecord result(RECORD_SIZE);
        memset(result.data, 0, result.size);
        memcpy(result.data + BIGINT_OFFSET, &big, sizeof(big));
        memcpy(result.data + FLOAT_OFFSET, &ratio, sizeof(ratio));
        memcpy(result.data + STRING_OFFSET, name.data(), std::min<size_t>(name.size(), 8));
        memcpy(result.data + DATETIME_OFFSET, &datetime, sizeof(datetime));
        return result;
    };
    std::vector<Condition> conditions;
    for (const auto &name : {"big", "ratio", "name", "created"}) {
        conditions.push_back(make_column_condition("t1", name, OP_EQ, "t2", name));
    }
    const int64_t timestamp = parse_datetime("2025-06-24 12:30:00");
    HashJoinExecutor executor(
        std::make_unique<MockTupleExecutor>(
            columns("t1"), std::vector<RmRecord>{record(7, 1.5F, "alpha", timestamp)}),
        std::make_unique<MockTupleExecutor>(
            columns("t2"), std::vector<RmRecord>{
                               record(7, 1.5F, "alpha", timestamp),
                               record(7, 1.5F, "beta", timestamp),
                           }),
        conditions, false, 4096);

    executor.beginTuple();
    ASSERT_FALSE(executor.is_end());
    EXPECT_NE(executor.Next(), nullptr);
    executor.nextTuple();
    EXPECT_TRUE(executor.is_end());
}

TEST(HashJoinExecutorTest, FallsBackWhenFixedMemoryCannotHoldHashTable) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    HashJoinExecutor executor(
        std::make_unique<MockTupleExecutor>(
            left_cols, std::vector<RmRecord>{make_join_record(1), make_join_record(2)}),
        std::make_unique<MockTupleExecutor>(
            right_cols, std::vector<RmRecord>{make_join_record(2), make_join_record(3)}),
        std::vector<Condition>{make_column_condition("t1", "id", OP_EQ, "t2", "id")},
        true, 1);

    executor.beginTuple();
    EXPECT_TRUE(executor.usingFallback());
    std::vector<std::pair<int, int>> pairs;
    for (; !executor.is_end(); executor.nextTuple()) {
        auto record = executor.Next();
        int left = 0;
        int right = 0;
        memcpy(&left, record->data, sizeof(left));
        memcpy(&right, record->data + sizeof(left), sizeof(right));
        pairs.emplace_back(left, right);
    }
    EXPECT_EQ(pairs, (std::vector<std::pair<int, int>>{{2, 2}}));
}

TEST(SortExecutorTest, SupportsMultiKeyOrderByAndLimit) {
    std::vector<ColMeta> cols = {
        {.tab_name = "orders", .name = "company", .type = TYPE_STRING, .len = 10, .offset = 0, .index = false},
        {.tab_name = "orders", .name = "order_number", .type = TYPE_INT, .len = 4, .offset = 10, .index = false},
    };
    std::vector<RmRecord> rows = {
        make_sort_record("AAA", 12),
        make_sort_record("ABB", 13),
        make_sort_record("ABC", 19),
        make_sort_record("ACA", 1),
    };

    std::vector<OrderByClause> order_bys = {
        {.col = {.tab_name = "orders", .col_name = "company"}, .is_desc = true},
        {.col = {.tab_name = "orders", .col_name = "order_number"}, .is_desc = false},
    };
    SortExecutor by_company(std::make_unique<MockTupleExecutor>(cols, rows), order_bys, false, 0);
    by_company.beginTuple();

    std::vector<std::string> companies;
    for (; !by_company.is_end(); by_company.nextTuple()) {
        auto record = by_company.Next();
        companies.emplace_back(record->data, record->data + 3);
    }
    EXPECT_EQ(companies, (std::vector<std::string>{"ACA", "ABC", "ABB", "AAA"}));

    std::vector<OrderByClause> by_number = {
        {.col = {.tab_name = "orders", .col_name = "order_number"}, .is_desc = false},
    };
    SortExecutor limited(std::make_unique<MockTupleExecutor>(cols, rows), by_number, true, 2);
    limited.beginTuple();

    std::vector<int> order_numbers;
    for (; !limited.is_end(); limited.nextTuple()) {
        auto record = limited.Next();
        int value = 0;
        memcpy(&value, record->data + 10, sizeof(value));
        order_numbers.push_back(value);
    }
    EXPECT_EQ(order_numbers, (std::vector<int>{1, 12}));
}

TEST(BlockNestedLoopJoinExecutorTest, SupportsMultipleBlocksAndNonEquiJoin) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "t_id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<RmRecord> left_rows = {
        make_join_record(1),
        make_join_record(2),
        make_join_record(3),
    };
    std::vector<RmRecord> right_rows = {
        make_join_record(2),
        make_join_record(3),
        make_join_record(4),
    };

    auto left_stats = std::make_shared<MockTupleExecutorStats>();
    Condition cond = make_column_condition("t1", "id", OP_LT, "t2", "t_id");

    NestedLoopJoinExecutor executor(std::make_unique<MockTupleExecutor>(left_cols, left_rows, left_stats),
                                    std::make_unique<MockTupleExecutor>(right_cols, right_rows), {cond},
                                    sizeof(int) * 2);

    auto pairs = collect_join_pairs(executor);
    EXPECT_EQ(pairs, (std::vector<std::pair<int, int>>{
        {1, 2}, {1, 3}, {1, 4}, {2, 3}, {2, 4}, {3, 4},
    }));
    EXPECT_EQ(left_stats->begin_count, 2);
}

TEST(BlockNestedLoopJoinExecutorTest, ClampsJoinBufferToSafeBounds) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "t_id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };

    NestedLoopJoinExecutor too_small(
        std::make_unique<MockTupleExecutor>(left_cols, std::vector<RmRecord>{make_join_record(1)}),
        std::make_unique<MockTupleExecutor>(right_cols, std::vector<RmRecord>{make_join_record(1)}),
        {}, 1);
    EXPECT_EQ(too_small.joinBufferSize(), sizeof(int));
    EXPECT_EQ(too_small.rightBlockCapacity(), 1);
    EXPECT_FALSE(too_small.isJoinBufferAllocated());

    NestedLoopJoinExecutor too_large(
        std::make_unique<MockTupleExecutor>(left_cols, std::vector<RmRecord>{make_join_record(1)}),
        std::make_unique<MockTupleExecutor>(right_cols, std::vector<RmRecord>{make_join_record(1)}),
        {}, 128ULL * 1024 * 1024);
    EXPECT_EQ(too_large.joinBufferSize(), 64ULL * 1024 * 1024);
    EXPECT_EQ(too_large.rightBlockCapacity(), 64ULL * 1024 * 1024 / sizeof(int));
    EXPECT_FALSE(too_large.isJoinBufferAllocated());
}

TEST(BlockNestedLoopJoinExecutorTest, AppliesJoinAndRightValueConditionsWithoutRescanningSingleBlock) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "t_id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<RmRecord> left_rows = {
        make_join_record(1), make_join_record(2), make_join_record(3),
    };
    std::vector<RmRecord> right_rows = {
        make_join_record(2), make_join_record(999), make_join_record(1000),
    };
    auto left_stats = std::make_shared<MockTupleExecutorStats>();

    std::vector<Condition> conds = {
        make_column_condition("t1", "id", OP_LT, "t2", "t_id"),
        make_int_value_condition("t2", "t_id", OP_LT, 1000),
    };
    NestedLoopJoinExecutor executor(std::make_unique<MockTupleExecutor>(left_cols, left_rows, left_stats),
                                    std::make_unique<MockTupleExecutor>(right_cols, right_rows), conds, 64);

    EXPECT_EQ(collect_join_pairs(executor), (std::vector<std::pair<int, int>>{
        {1, 2}, {1, 999}, {2, 999}, {3, 999},
    }));
    EXPECT_EQ(left_stats->begin_count, 1);
}

TEST(BlockNestedLoopJoinExecutorTest, EmptyRightInputDoesNotScanLeftInput) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "t_id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    auto left_stats = std::make_shared<MockTupleExecutorStats>();
    NestedLoopJoinExecutor executor(
        std::make_unique<MockTupleExecutor>(
            left_cols, std::vector<RmRecord>{make_join_record(1), make_join_record(2)}, left_stats),
        std::make_unique<MockTupleExecutor>(right_cols, std::vector<RmRecord>{}), {}, 8);

    executor.beginTuple();
    EXPECT_TRUE(executor.is_end());
    EXPECT_EQ(executor.Next(), nullptr);
    EXPECT_EQ(left_stats->begin_count, 0);
}

TEST(BlockNestedLoopJoinExecutorTest, EmptyLeftInputStopsAfterFirstRightBlock) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "t_id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    auto right_stats = std::make_shared<MockTupleExecutorStats>();
    NestedLoopJoinExecutor executor(
        std::make_unique<MockTupleExecutor>(left_cols, std::vector<RmRecord>{}),
        std::make_unique<MockTupleExecutor>(
            right_cols,
            std::vector<RmRecord>{make_join_record(1), make_join_record(2), make_join_record(3)},
            right_stats),
        {}, sizeof(int));

    executor.beginTuple();
    EXPECT_TRUE(executor.is_end());
    EXPECT_EQ(right_stats->begin_count, 1);
    EXPECT_EQ(right_stats->next_count, 1);
}

TEST(BlockNestedLoopJoinExecutorTest, SupportsAllComparisonOperatorsAndCartesianProduct) {
    std::vector<ColMeta> left_cols = {
        {.tab_name = "t1", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> right_cols = {
        {.tab_name = "t2", .name = "t_id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    const std::vector<std::pair<CompOp, size_t>> cases = {
        {OP_EQ, 1}, {OP_NE, 0}, {OP_LT, 0}, {OP_GT, 0}, {OP_LE, 1}, {OP_GE, 1},
    };
    for (const auto &[op, expected_count] : cases) {
        NestedLoopJoinExecutor executor(
            std::make_unique<MockTupleExecutor>(left_cols, std::vector<RmRecord>{make_join_record(2)}),
            std::make_unique<MockTupleExecutor>(right_cols, std::vector<RmRecord>{make_join_record(2)}),
            {make_column_condition("t1", "id", op, "t2", "t_id")}, 4);
        EXPECT_EQ(collect_join_pairs(executor).size(), expected_count);
    }

    NestedLoopJoinExecutor cartesian(
        std::make_unique<MockTupleExecutor>(
            left_cols, std::vector<RmRecord>{make_join_record(1), make_join_record(2)}),
        std::make_unique<MockTupleExecutor>(
            right_cols, std::vector<RmRecord>{make_join_record(3), make_join_record(4)}),
        {}, 4);
    EXPECT_EQ(collect_join_pairs(cartesian), (std::vector<std::pair<int, int>>{
        {1, 3}, {1, 4}, {2, 3}, {2, 4},
    }));
}

TEST(BlockNestedLoopJoinExecutorTest, CanBeRestartedAndNested) {
    std::vector<ColMeta> t1_cols = {
        {.tab_name = "t1", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> t2_cols = {
        {.tab_name = "t2", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };
    std::vector<ColMeta> t3_cols = {
        {.tab_name = "t3", .name = "id", .type = TYPE_INT, .len = 4, .offset = 0, .index = false},
    };

    auto inner = std::make_unique<NestedLoopJoinExecutor>(
        std::make_unique<MockTupleExecutor>(
            t1_cols, std::vector<RmRecord>{make_join_record(1), make_join_record(2)}),
        std::make_unique<MockTupleExecutor>(
            t2_cols, std::vector<RmRecord>{make_join_record(2), make_join_record(3)}),
        std::vector<Condition>{make_column_condition("t1", "id", OP_LT, "t2", "id")}, 4);
    NestedLoopJoinExecutor outer(
        std::move(inner),
        std::make_unique<MockTupleExecutor>(
            t3_cols, std::vector<RmRecord>{make_join_record(3), make_join_record(4)}),
        {make_column_condition("t2", "id", OP_LT, "t3", "id")}, 8);

    auto collect_triples = [&]() {
        std::vector<std::vector<int>> triples;
        for (outer.beginTuple(); !outer.is_end(); outer.nextTuple()) {
            auto record = outer.Next();
            std::vector<int> values(3);
            memcpy(&values[0], record->data, sizeof(int));
            memcpy(&values[1], record->data + sizeof(int), sizeof(int));
            memcpy(&values[2], record->data + sizeof(int) * 2, sizeof(int));
            triples.push_back(values);
        }
        std::sort(triples.begin(), triples.end());
        return triples;
    };

    const std::vector<std::vector<int>> expected = {
        {1, 2, 3}, {1, 2, 4}, {1, 3, 4}, {2, 3, 4},
    };
    EXPECT_EQ(collect_triples(), expected);
    EXPECT_EQ(collect_triples(), expected);
}

// 创建BufferPoolManager
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager.get());

std::unordered_map<int, char *> mock;  // fd -> buffer

char *mock_get_page(int fd, int page_no) { return &mock[fd][page_no * PAGE_SIZE]; }

void check_disk(int fd, int page_no) {
    char buf[PAGE_SIZE];
    disk_manager->read_page(fd, page_no, buf, PAGE_SIZE);
    char *mock_buf = mock_get_page(fd, page_no);
    assert(memcmp(buf, mock_buf, PAGE_SIZE) == 0);
}

void check_disk_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
}

void check_cache(int fd, int page_no) {
    Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
    char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE];
    assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);
    buffer_pool_manager->unpin_page(PageId{fd, page_no}, false);
}

void check_cache_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_cache(fd, page_no);
        }
    }
}

void rand_buf(int size, char *buf) {
    for (int i = 0; i < size; i++) {
        int rand_ch = rand() & 0xff;
        buf[i] = rand_ch;
    }
}

int rand_fd() {
    assert(mock.size() == MAX_FILES);
    int fd_idx = rand() % MAX_FILES;
    auto it = mock.begin();
    for (int i = 0; i < fd_idx; i++) {
        it++;
    }
    return it->first;
}

struct rid_hash_t {
    size_t operator()(const Rid &rid) const { return (rid.page_no << 16) | rid.slot_no; }
};

struct rid_equal_t {
    bool operator()(const Rid &x, const Rid &y) const { return x.page_no == y.page_no && x.slot_no == y.slot_no; }
};

void check_equal(const RmFileHandle *file_handle,
                 const std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> &mock) {
    // Test all records
    for (auto &entry : mock) {
        Rid rid = entry.first;
        auto mock_buf = (char *)entry.second.c_str();
        auto rec = file_handle->get_record(rid, nullptr);
        assert(memcmp(mock_buf, rec->data, file_handle->file_hdr_.record_size) == 0);
    }
    // Randomly get record
    for (int i = 0; i < 10; i++) {
        Rid rid = {.page_no = 1 + rand() % (file_handle->file_hdr_.num_pages - 1),
                   .slot_no = rand() % file_handle->file_hdr_.num_records_per_page};
        bool mock_exist = mock.count(rid) > 0;
        bool rm_exist = file_handle->is_record(rid);
        assert(rm_exist == mock_exist);
    }
    // Test RM scan
    size_t num_records = 0;
    for (RmScan scan(file_handle); !scan.is_end(); scan.next()) {
        assert(mock.count(scan.rid()) > 0);
        auto rec = file_handle->get_record(scan.rid(), nullptr);
        assert(memcmp(rec->data, mock.at(scan.rid()).c_str(), file_handle->file_hdr_.record_size) == 0);
        num_records++;
    }
    assert(num_records == mock.size());
}

// std::cout can call this, for example: std::cout << rid
std::ostream &operator<<(std::ostream &os, const Rid &rid) {
    return os << '(' << rid.page_no << ", " << rid.slot_no << ')';
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_BIG，记录其文件描述符fd */

class BigStorageTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_BIG)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_BIG);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_BIG);
        assert(disk_manager_->is_file(TEST_FILE_NAME_BIG));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_BIG);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_BIG);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST(LRUReplacerTest, SampleTest) {
    LRUReplacer lru_replacer(7);

    // Scenario: unpin six elements, i.e. add them to the replacer.
    lru_replacer.unpin(1);
    lru_replacer.unpin(2);
    lru_replacer.unpin(3);
    lru_replacer.unpin(4);
    lru_replacer.unpin(5);
    lru_replacer.unpin(6);
    lru_replacer.unpin(1);
    EXPECT_EQ(6, lru_replacer.Size());

    // Scenario: get three victims from the lru.
    int value;
    lru_replacer.victim(&value);
    EXPECT_EQ(1, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(2, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(3, value);

    // Scenario: pin elements in the replacer.
    // Note that 3 has already been victimized, so pinning 3 should have no effect.
    lru_replacer.pin(3);
    lru_replacer.pin(4);
    EXPECT_EQ(2, lru_replacer.Size());

    // Scenario: unpin 4. We expect that the reference bit of 4 will be set to 1.
    lru_replacer.unpin(4);

    // Scenario: continue looking for victims. We expect these victims.
    lru_replacer.victim(&value);
    EXPECT_EQ(5, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(6, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(4, value);
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME，记录其文件描述符fd */
class BufferPoolManagerTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME)) {
            disk_manager_->destroy_file(TEST_FILE_NAME);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME);
        assert(disk_manager_->is_file(TEST_FILE_NAME));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

// NOLINTNEXTLINE
TEST_F(BufferPoolManagerTest, SampleTest) {
    // create BufferPoolManager
    const size_t buffer_pool_size = 10;
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    auto bpm = std::make_unique<BufferPoolManager>(buffer_pool_size, disk_manager);
    // create tmp PageId
    int fd = BufferPoolManagerTest::fd_;
    PageId page_id_temp = {.fd = fd, .page_no = INVALID_PAGE_ID};
    auto *page0 = bpm->new_page(&page_id_temp);

    // Scenario: The buffer pool is empty. We should be able to create a new page.
    ASSERT_NE(nullptr, page0);
    EXPECT_EQ(0, page_id_temp.page_no);

    // Scenario: Once we have a page, we should be able to read and write content.
    snprintf(page0->get_data(), sizeof(page0->get_data()), "Hello");
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));

    // Scenario: We should be able to create new pages until we fill up the buffer pool.
    for (size_t i = 1; i < buffer_pool_size; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: Once the buffer pool is full, we should not be able to create any new pages.
    for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
        EXPECT_EQ(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: After unpinning pages {0, 1, 2, 3, 4} and pinning another 4 new pages,
    // there would still be one cache frame left for reading page 0.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(true, bpm->unpin_page(PageId{fd, i}, true));
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: We should be able to fetch the data we wrote a while ago.
    page0 = bpm->fetch_page(PageId{fd, 0});
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));
    EXPECT_EQ(true, bpm->unpin_page(PageId{fd, 0}, true));
    // new_page again, and now all buffers are pinned. Page 0 would be failed to fetch.
    EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    EXPECT_EQ(nullptr, bpm->fetch_page(PageId{fd, 0}));

    bpm->flush_all_pages(fd);
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_CCUR，记录其文件描述符fd */

// Add by jiawen
class BufferPoolManagerConcurrencyTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_CCUR)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_CCUR);
        assert(disk_manager_->is_file(TEST_FILE_NAME_CCUR));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_CCUR);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST_F(BufferPoolManagerConcurrencyTest, ConcurrencyTest) {
    const int num_threads = 5;
    const int num_runs = 50;

    // get fd
    int fd = BufferPoolManagerConcurrencyTest::fd_;

    for (int run = 0; run < num_runs; run++) {
        // create BufferPoolManager
        auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
        std::shared_ptr<BufferPoolManager> bpm{new BufferPoolManager(50, disk_manager)};

        std::vector<std::thread> threads;
        for (int tid = 0; tid < num_threads; tid++) {
            threads.push_back(std::thread([&bpm, fd]() {  // NOLINT
                PageId temp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
                std::vector<PageId> page_ids;
                for (int i = 0; i < 10; i++) {
                    auto new_page = bpm->new_page(&temp_page_id);
                    EXPECT_NE(nullptr, new_page);
                    ASSERT_NE(nullptr, new_page);
                    strcpy(new_page->get_data(), std::to_string(temp_page_id.page_no).c_str());  // NOLINT
                    page_ids.push_back(temp_page_id);
                }
                for (int i = 0; i < 10; i++) {
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[i], true));
                }
                for (int j = 0; j < 10; j++) {
                    auto page = bpm->fetch_page(page_ids[j]);
                    EXPECT_NE(nullptr, page);
                    ASSERT_NE(nullptr, page);
                    EXPECT_EQ(0, std::strcmp(std::to_string(page_ids[j].page_no).c_str(), (page->get_data())));
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[j], true));
                }
                for (int j = 0; j < 10; j++) {
                    EXPECT_EQ(1, bpm->delete_page(page_ids[j]));
                }
                bpm->flush_all_pages(fd);  // add this test by jiawen
            }));
        }  // end loop tid=[0,num_threads)

        for (int i = 0; i < num_threads; i++) {
            threads[i].join();
        }
    }  // end loop run=[0,num_runs)
}

// TODO: fix detected memory leaks found by Google Test
TEST(StorageTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    /** Test disk_manager */
    std::vector<std::string> filenames(MAX_FILES);  // MAX_FILES=32
    std::unordered_map<int, std::string> fd2name;
    for (size_t i = 0; i < filenames.size(); i++) {
        auto &filename = filenames[i];
        filename = std::to_string(i) + ".txt";
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // open without create
        try {
            disk_manager->open_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }

        disk_manager->create_file(filename);
        assert(disk_manager->is_file(filename));
        try {
            disk_manager->create_file(filename);
            assert(false);
        } catch (const FileExistsError &e) {
        }

        // open file
        int fd = disk_manager->open_file(filename);
        char *tmp = new char[PAGE_SIZE * MAX_PAGES];  // TODO: fix error in detected memory leaks

        mock[fd] = tmp;
        fd2name[fd] = filename;

        disk_manager->set_fd2pageno(fd, 0);  // diskmanager在fd对应的文件中从0开始分配page_no
    }

    /** Test buffer_pool_manager*/
    int num_pages = 0;
    char init_buf[PAGE_SIZE];
    for (auto &fh : mock) {
        int fd = fh.first;
        for (page_id_t i = 0; i < MAX_PAGES; i++) {
            rand_buf(PAGE_SIZE, init_buf);  // 将init_buf填充PAGE_SIZE个字节的随机数据

            PageId tmp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
            Page *page = buffer_pool_manager->new_page(&tmp_page_id);
            int page_no = tmp_page_id.page_no;
            assert(page_no != INVALID_PAGE_ID);
            assert(page_no == i);

            memcpy(page->get_data(), init_buf, PAGE_SIZE);
            buffer_pool_manager->unpin_page(PageId{fd, page_no}, true);

            char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE]
            memcpy(mock_buf, init_buf, PAGE_SIZE);

            num_pages++;

            check_cache(fd, page_no);  // 调用了fetch_page, unpin_page
        }
    }
    check_cache_all();

    assert(num_pages == TEST_BUFFER_POOL_SIZE);

    /** Test flush_all_pages() */
    // Flush and test disk
    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    for (int r = 0; r < 10000; r++) {
        int fd = rand_fd();
        int page_no = rand() % MAX_PAGES;
        // fetch page
        Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
        char *mock_buf = mock_get_page(fd, page_no);
        assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);

        // modify
        rand_buf(PAGE_SIZE, init_buf);
        memcpy(page->get_data(), init_buf, PAGE_SIZE);
        memcpy(mock_buf, init_buf, PAGE_SIZE);

        buffer_pool_manager->unpin_page(page->get_page_id(), true);
        // BufferPool::mark_dirty(page);

        // flush
        if (rand() % 10 == 0) {
            buffer_pool_manager->flush_page(page->get_page_id());
            check_disk(fd, page_no);
        }
        // flush entire file
        if (rand() % 100 == 0) {
            buffer_pool_manager->flush_all_pages(fd);
        }
        // re-open file
        if (rand() % 100 == 0) {
            disk_manager->close_file(fd);
            auto filename = fd2name[fd];
            char *buf = mock[fd];
            fd2name.erase(fd);
            mock.erase(fd);
            int new_fd = disk_manager->open_file(filename);
            mock[new_fd] = buf;
            fd2name[new_fd] = filename;
        }
        // assert equal in cache
        check_cache(fd, page_no);
    }
    check_cache_all();

    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    // close and destroy files
    for (auto &entry : fd2name) {
        int fd = entry.first;
        auto &filename = entry.second;
        disk_manager->close_file(fd);
        disk_manager->destroy_file(filename);
        try {
            disk_manager->destroy_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }
    }
}

TEST(RecordManagerTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    // 创建RmManager类的对象rm_manager
    auto disk_manager = std::make_unique<DiskManager>();
    auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
    auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());

    std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> mock;

    std::string filename = "abc.txt";

    int record_size = 4 + rand() % 256;  // 元组大小随便设置，只要不超过RM_MAX_RECORD_SIZE
    // test files
    {
        // 删除残留的同名文件
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // 将file header写入到磁盘中的filename文件
        rm_manager->create_file(filename, record_size);
        // 将磁盘中的filename文件读出到内存中的file handle的file header
        std::unique_ptr<RmFileHandle> file_handle = rm_manager->open_file(filename);
        // 检查filename文件在内存中的file header的参数
        assert(file_handle->file_hdr_.record_size == record_size);
        assert(file_handle->file_hdr_.first_free_page_no == RM_NO_PAGE);
        assert(file_handle->file_hdr_.num_pages == 1);

        int max_bytes = file_handle->file_hdr_.record_size * file_handle->file_hdr_.num_records_per_page +
                        file_handle->file_hdr_.bitmap_size + (int)sizeof(RmPageHdr);
        assert(max_bytes <= PAGE_SIZE);
        int rand_val = rand();
        file_handle->file_hdr_.num_pages = rand_val;
        rm_manager->close_file(file_handle.get());

        // reopen file
        file_handle = rm_manager->open_file(filename);
        assert(file_handle->file_hdr_.num_pages == rand_val);
        rm_manager->close_file(file_handle.get());
        rm_manager->destroy_file(filename);
    }
    // test pages
    rm_manager->create_file(filename, record_size);
    auto file_handle = rm_manager->open_file(filename);

    char write_buf[PAGE_SIZE];
    size_t add_cnt = 0;
    size_t upd_cnt = 0;
    size_t del_cnt = 0;
    for (int round = 0; round < 1000; round++) {
        double insert_prob = 1. - mock.size() / 250.;
        double dice = rand() * 1. / RAND_MAX;
        if (mock.empty() || dice < insert_prob) {
            rand_buf(file_handle->file_hdr_.record_size, write_buf);
            Rid rid = file_handle->insert_record(write_buf, nullptr);
            mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
            add_cnt++;
            //            std::cout << "insert " << rid << '\n'; // operator<<(cout,rid)
        } else {
            // update or erase random rid
            int rid_idx = rand() % mock.size();
            auto it = mock.begin();
            for (int i = 0; i < rid_idx; i++) {
                it++;
            }
            auto rid = it->first;
            if (rand() % 2 == 0) {
                // update
                rand_buf(file_handle->file_hdr_.record_size, write_buf);
                file_handle->update_record(rid, write_buf, nullptr);
                mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
                upd_cnt++;
                //                std::cout << "update " << rid << '\n';
            } else {
                // erase
                file_handle->delete_record(rid, nullptr);
                mock.erase(rid);
                del_cnt++;
                //                std::cout << "delete " << rid << '\n';
            }
        }
        // Randomly re-open file
        if (round % 50 == 0) {
            rm_manager->close_file(file_handle.get());
            file_handle = rm_manager->open_file(filename);
        }
        check_equal(file_handle.get(), mock);
    }
    assert(mock.size() == add_cnt - del_cnt);
    std::cout << "insert " << add_cnt << '\n' << "delete " << del_cnt << '\n' << "update " << upd_cnt << '\n';
    // clean up
    rm_manager->close_file(file_handle.get());
    rm_manager->destroy_file(filename);
}
