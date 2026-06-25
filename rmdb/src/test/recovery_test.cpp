#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

#include "gtest/gtest.h"
#include "recovery/log_manager.h"
#include "recovery/log_recovery.h"
#include "record/rm_manager.h"
#include "system/sm_manager.h"

namespace {

class ScopedTempDir {
   public:
    ScopedTempDir() {
        if (getcwd(old_cwd_, sizeof(old_cwd_)) == nullptr) {
            throw std::runtime_error("getcwd failed");
        }
        char path[] = "/tmp/rmdb_recovery_test_XXXXXX";
        char *created = mkdtemp(path);
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
        if (chdir(path_.c_str()) < 0) {
            throw std::runtime_error("chdir failed");
        }
    }

    ~ScopedTempDir() {
        chdir(old_cwd_);
        unlink((path_ + "/" + LOG_FILE_NAME).c_str());
        unlink((path_ + "/" + DB_META_NAME).c_str());
        unlink((path_ + "/output.txt").c_str());
        unlink((path_ + "/t").c_str());
        unlink((path_ + "/t#id.idx").c_str());
        rmdir(path_.c_str());
    }

   private:
    char old_cwd_[PATH_MAX]{};
    std::string path_;
};

class RecoveryTestEnv {
   public:
    RecoveryTestEnv()
        : bpm_(16, &disk_manager_),
          rm_manager_(&disk_manager_, &bpm_),
          ix_manager_(&disk_manager_, &bpm_),
          sm_manager_(&disk_manager_, &bpm_, &rm_manager_, &ix_manager_),
          log_manager_(&disk_manager_),
          recovery_(&disk_manager_, &bpm_, &sm_manager_, &log_manager_) {
        disk_manager_.create_file(LOG_FILE_NAME);
        rm_manager_.create_file("t", sizeof(int));

        TabMeta tab;
        tab.name = "t";
        tab.cols.push_back(ColMeta{"t", "id", TYPE_INT, sizeof(int), 0, false});
        std::ofstream meta(DB_META_NAME);
        meta << "recovery_test\n1\n" << tab << '\n';
        meta.close();

        log_manager_.initialize();
        bpm_.set_log_manager(&log_manager_);
        sm_manager_.load_db();
    }

    ~RecoveryTestEnv() {
        try {
            sm_manager_.close_db();
        } catch (...) {
        }
    }

    void append_insert(txn_id_t txn_id, const Rid &rid, const char *data, size_t size, bool commit_txn) {
        BeginLogRecord begin_record(txn_id);
        lsn_t prev_lsn = log_manager_.append(begin_record, INVALID_LSN);

        LogRecord insert_record(INSERT, txn_id);
        insert_record.set_object_name("t");
        insert_record.set_rid(rid);
        insert_record.set_after_image(data, size);
        prev_lsn = log_manager_.append(insert_record, prev_lsn);

        if (commit_txn) {
            CommitLogRecord commit_record(txn_id);
            log_manager_.append(commit_record, prev_lsn);
        }
        log_manager_.flush_log_to_disk(true);
    }

    void recover() {
        recovery_.analyze();
        recovery_.recover_ddl();
        recovery_.redo();
        recovery_.undo();
    }

    int log_file_size() { return disk_manager_.get_file_size(LOG_FILE_NAME); }

    void append_invalid_log_tail() {
        char tail[] = {1, 2, 3};
        disk_manager_.write_log(tail, sizeof(tail));
        disk_manager_.sync_log();
    }

    std::vector<std::unique_ptr<LogRecord>> read_all_valid_logs() {
        return log_manager_.read_all_valid(true);
    }

    void create_id_index() {
        sm_manager_.create_index("t", std::vector<std::string>{"id"}, nullptr);
    }

    std::vector<Rid> lookup_id(int value) {
        std::vector<std::string> columns{"id"};
        std::string name = ix_manager_.get_index_name("t", columns);
        std::vector<Rid> result;
        sm_manager_.ihs_.at(name)->get_value(reinterpret_cast<const char *>(&value), &result, nullptr);
        return result;
    }

    RmFileHandle *table() { return sm_manager_.fhs_.at("t").get(); }

   private:
    ScopedTempDir temp_dir_;
    DiskManager disk_manager_;
    BufferPoolManager bpm_;
    RmManager rm_manager_;
    IxManager ix_manager_;
    SmManager sm_manager_;
    LogManager log_manager_;
    RecoveryManager recovery_;
};

}  // namespace

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

TEST(RecoveryRecordTest, MissingLoggedPageIsCreatedAtRid) {
    RecoveryTestEnv env;
    Rid rid{RM_FIRST_RECORD_PAGE, 0};
    int value = 42;

    EXPECT_FALSE(env.table()->is_record(rid));
    EXPECT_FALSE(env.table()->is_record(Rid{RM_FIRST_RECORD_PAGE, -1}));
    EXPECT_FALSE(env.table()->is_record(Rid{RM_FIRST_RECORD_PAGE + 10, 0}));

    env.append_insert(1, rid, reinterpret_cast<const char *>(&value), sizeof(value), true);
    env.recover();

    ASSERT_TRUE(env.table()->is_record(rid));
    auto record = env.table()->get_record(rid, nullptr);
    int recovered_value;
    memcpy(&recovered_value, record->data, sizeof(recovered_value));
    EXPECT_EQ(recovered_value, value);

    env.recover();
    ASSERT_TRUE(env.table()->is_record(rid));
    record = env.table()->get_record(rid, nullptr);
    memcpy(&recovered_value, record->data, sizeof(recovered_value));
    EXPECT_EQ(recovered_value, value);
}

TEST(RecoveryRecordTest, UncommittedInsertDoesNotCreateMissingPage) {
    RecoveryTestEnv env;
    Rid rid{RM_FIRST_RECORD_PAGE, 0};
    int value = 7;

    env.append_insert(2, rid, reinterpret_cast<const char *>(&value), sizeof(value), false);
    env.recover();

    EXPECT_FALSE(env.table()->is_record(rid));
    EXPECT_EQ(env.table()->get_file_hdr().num_pages, RM_FIRST_RECORD_PAGE);
}

TEST(RecoveryRecordTest, RejectsTupleImageWithWrongSize) {
    RecoveryTestEnv env;
    Rid rid{RM_FIRST_RECORD_PAGE, 0};
    char value = 1;

    env.append_insert(3, rid, &value, sizeof(value), true);
    EXPECT_THROW(env.recover(), InternalError);
}

TEST(RecoveryIndexTest, RebuildsIndexFromRecoveredBaseTable) {
    RecoveryTestEnv env;
    Rid rid{RM_FIRST_RECORD_PAGE, 0};
    int value = 88;

    env.create_id_index();
    env.append_insert(5, rid, reinterpret_cast<const char *>(&value), sizeof(value), true);
    env.recover();

    auto result = env.lookup_id(value);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.front(), rid);
}

TEST(RecoveryLogTest, TruncatesIncompleteLogTail) {
    RecoveryTestEnv env;
    Rid rid{RM_FIRST_RECORD_PAGE, 0};
    int value = 9;

    env.append_insert(4, rid, reinterpret_cast<const char *>(&value), sizeof(value), true);
    int valid_size = env.log_file_size();
    env.append_invalid_log_tail();
    ASSERT_EQ(env.log_file_size(), valid_size + 3);

    auto records = env.read_all_valid_logs();
    EXPECT_EQ(records.size(), 3);
    EXPECT_EQ(env.log_file_size(), valid_size);
}
