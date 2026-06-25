#include <cstring>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "recovery/log_manager.h"

TEST(RecoveryLogTest, RoundTripsUpdateImagesAndChain) {
    RmRecord before(sizeof(int));
    RmRecord after(sizeof(int));
    int old_value = 7;
    int new_value = 11;
    memcpy(before.data, &old_value, sizeof(old_value));
    memcpy(after.data, &new_value, sizeof(new_value));

    UpdateLogRecord source(42, before, after, Rid{3, 5}, "t");
    source.lsn_ = 128;
    source.prev_lsn_ = 64;
    source.log_tot_len_ = source.serialized_size();
    std::vector<char> bytes(source.log_tot_len_);
    source.serialize(bytes.data());

    auto decoded = LogRecord::deserialize(bytes.data(), bytes.size());
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->log_type_, UPDATE);
    EXPECT_EQ(decoded->lsn_, 128);
    EXPECT_EQ(decoded->prev_lsn_, 64);
    EXPECT_EQ(decoded->log_tid_, 42);
    EXPECT_EQ(decoded->object_name_, "t");
    EXPECT_EQ(decoded->rid_, (Rid{3, 5}));
    ASSERT_EQ(decoded->before_image_.size(), sizeof(int));
    ASSERT_EQ(decoded->after_image_.size(), sizeof(int));
    EXPECT_EQ(memcmp(decoded->before_image_.data(), before.data, sizeof(int)), 0);
    EXPECT_EQ(memcmp(decoded->after_image_.data(), after.data, sizeof(int)), 0);
}

TEST(RecoveryLogTest, RejectsIncompleteTail) {
    BeginLogRecord source(9);
    source.lsn_ = 0;
    source.log_tot_len_ = source.serialized_size();
    std::vector<char> bytes(source.log_tot_len_);
    source.serialize(bytes.data());

    ASSERT_GT(bytes.size(), static_cast<size_t>(LOG_HEADER_SIZE));
    EXPECT_EQ(LogRecord::deserialize(bytes.data(), bytes.size() - 1), nullptr);
}

TEST(RecoveryLogTest, RoundTripsPhysicalIndexKey) {
    const char key[] = {'a', '\0', 'z', '\1'};
    LogRecord source(INDEX_INSERT, 5);
    source.set_object_name("t#c.idx");
    source.set_key(key, sizeof(key));
    source.set_rid(Rid{8, 2});
    source.lsn_ = 17;
    source.log_tot_len_ = source.serialized_size();
    std::vector<char> bytes(source.log_tot_len_);
    source.serialize(bytes.data());

    auto decoded = LogRecord::deserialize(bytes.data(), bytes.size());
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->object_name_, "t#c.idx");
    EXPECT_EQ(decoded->key_, std::vector<char>(key, key + sizeof(key)));
    EXPECT_EQ(decoded->rid_, (Rid{8, 2}));
}
