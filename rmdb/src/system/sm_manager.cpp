/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

namespace {

std::string meta_snapshot(const DbMeta &db) {
    std::ostringstream out;
    out << db;
    return out.str();
}

std::string ddl_tombstone(txn_id_t txn_id, const std::string &name) {
    return ".rmdb_drop_" + std::to_string(txn_id) + "_" + name;
}

void append_ddl_log(Context *context, LogType type, const std::string &object_name,
                    const std::string &aux_name, const DbMeta &before, const DbMeta &after) {
    if (context == nullptr || context->txn_ == nullptr || context->log_mgr_ == nullptr) return;
    LogRecord log(type, context->txn_->get_transaction_id());
    log.set_object_name(object_name);
    log.set_aux_name(aux_name);
    std::string before_text = meta_snapshot(before);
    std::string after_text = meta_snapshot(after);
    log.set_before_image(before_text.data(), before_text.size());
    log.set_after_image(after_text.data(), after_text.size());
    lsn_t lsn = context->log_mgr_->append(log, context->txn_->get_prev_lsn());
    context->txn_->set_prev_lsn(lsn);
    // DDL mutates directory entries and files outside the buffer pool, so its
    // prepare record must be stable before the first filesystem change.
    context->log_mgr_->flush_up_to(lsn);
}

}  // namespace

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::enter_db(const std::string &db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
}

void SmManager::load_db() {
    std::ifstream ifs(DB_META_NAME);
    if (!ifs.is_open()) {
        throw FileNotFoundError(DB_META_NAME);
    }
    db_ = DbMeta();
    ifs >> db_;

    fhs_.clear();
    ihs_.clear();
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        fhs_.emplace(tab.name, rm_manager_->open_file(tab.name));
        for (auto &index : tab.indexes) {
            if (!ix_manager_->exists(tab.name, index.cols)) {
                std::vector<std::string> col_names;
                col_names.reserve(index.cols.size());
                for (const auto &col : index.cols) {
                    col_names.push_back(col.name);
                }
                throw IndexNotFoundError(tab.name, col_names);
            }
            ihs_.emplace(ix_manager_->get_index_name(tab.name, index.cols),
                         ix_manager_->open_index(tab.name, index.cols));
        }
    }

    std::ofstream outfile("output.txt", std::ios::out | std::ios::trunc);
    outfile.close();
}

void SmManager::open_db(const std::string& db_name) {
    enter_db(db_name);
    load_db();
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    flush_meta();
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    std::fstream outfile("output.txt", std::ios::out | std::ios::app);
    if (!outfile.is_open()) {
        throw FileNotFoundError("output.txt");
    }
    RecordPrinter printer(3);
    for (const auto &index : tab.indexes) {
        std::string cols = "(";
        for (size_t i = 0; i < index.cols.size(); ++i) {
            if (i > 0) {
                cols += ",";
            }
            cols += index.cols[i].name;
        }
        cols += ")";
        printer.print_record({tab_name, "unique", cols}, context);
        outfile << "| " << tab_name << " | unique | " << cols << " |\n";
    }
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    std::lock_guard<std::mutex> schema_guard(schema_latch_);
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    DbMeta before = db_;
    DbMeta after = db_;
    after.tabs_[tab_name] = tab;
    append_ddl_log(context, CREATE_TABLE, tab_name, "", before, after);

    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_ = after;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    std::lock_guard<std::mutex> schema_guard(schema_latch_);
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    if (context != nullptr && context->txn_ != nullptr) {
        context->lock_mgr_->lock_exclusive_on_table(context->txn_, fhs_.at(tab_name)->GetFd());
    }
    DbMeta before = db_;
    DbMeta after = db_;
    after.tabs_.erase(tab_name);
    txn_id_t txn_id = context == nullptr || context->txn_ == nullptr
                          ? INVALID_TXN_ID
                          : context->txn_->get_transaction_id();
    std::string tombstone_prefix = ".rmdb_drop_" + std::to_string(txn_id) + "_";
    append_ddl_log(context, DROP_TABLE, tab_name, tombstone_prefix, before, after);

    auto &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto ix_name = ix_manager_->get_index_name(tab_name, index.cols);
        auto ih = ihs_.find(ix_name);
        if (ih != ihs_.end()) {
            ix_manager_->close_index(ih->second.get());
            ihs_.erase(ih);
        }
        if (disk_manager_->is_file(ix_name)) {
            disk_manager_->rename_file(ix_name, ddl_tombstone(txn_id, ix_name));
        }
    }

    auto fh = fhs_.find(tab_name);
    if (fh != fhs_.end()) {
        rm_manager_->close_file(fh->second.get());
        fhs_.erase(fh);
    }
    if (disk_manager_->is_file(tab_name)) {
        disk_manager_->rename_file(tab_name, ddl_tombstone(txn_id, tab_name));
    }
    db_ = after;
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    std::lock_guard<std::mutex> schema_guard(schema_latch_);
    TabMeta &tab = db_.get_table(tab_name);
    if (context != nullptr && context->txn_ != nullptr) {
        context->lock_mgr_->lock_exclusive_on_table(context->txn_, fhs_.at(tab_name)->GetFd());
    }
    if (col_names.empty()) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    IndexMeta index;
    index.tab_name = tab_name;
    index.col_num = static_cast<int>(col_names.size());
    index.col_tot_len = 0;
    for (const auto &name : col_names) {
        ColMeta col = *tab.get_col(name);
        index.col_tot_len += col.len;
        index.cols.push_back(col);
    }

    DbMeta before = db_;
    DbMeta after = db_;
    TabMeta &after_tab = after.get_table(tab_name);
    after_tab.indexes.push_back(index);
    for (auto &col : after_tab.cols) {
        if (std::find(col_names.begin(), col_names.end(), col.name) != col_names.end()) {
            col.index = true;
        }
    }
    append_ddl_log(context, CREATE_INDEX, ix_manager_->get_index_name(tab_name, index.cols),
                   tab_name, before, after);

    ix_manager_->create_index(tab_name, index.cols);
    std::unique_ptr<IxIndexHandle> ih;
    try {
        ih = ix_manager_->open_index(tab_name, index.cols);
        RmFileHandle *fh = fhs_.at(tab_name).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            Rid rid = scan.rid();
            auto rec = fh->get_record(rid, context);
            std::vector<char> key(index.col_tot_len);
            int offset = 0;
            for (const auto &col : index.cols) {
                memcpy(key.data() + offset, rec->data + col.offset, col.len);
                offset += col.len;
            }
            ih->insert_entry(key.data(), rid, context == nullptr ? nullptr : context->txn_);
        }
    } catch (...) {
        if (ih != nullptr) {
            ix_manager_->close_index(ih.get());
            ih.reset();
        }
        if (ix_manager_->exists(tab_name, index.cols)) {
            ix_manager_->destroy_index(tab_name, index.cols);
        }
        throw;
    }

    std::string ix_name = ix_manager_->get_index_name(tab_name, index.cols);
    ihs_.emplace(ix_name, std::move(ih));
    db_ = after;
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    std::lock_guard<std::mutex> schema_guard(schema_latch_);
    TabMeta &tab = db_.get_table(tab_name);
    if (context != nullptr && context->txn_ != nullptr) {
        context->lock_mgr_->lock_exclusive_on_table(context->txn_, fhs_.at(tab_name)->GetFd());
    }
    auto index_it = tab.get_index_meta(col_names);
    std::vector<ColMeta> cols = index_it->cols;
    std::string ix_name = ix_manager_->get_index_name(tab_name, cols);
    DbMeta before = db_;
    DbMeta after = db_;
    TabMeta &after_tab = after.get_table(tab_name);
    auto after_index = after_tab.get_index_meta(col_names);
    after_tab.indexes.erase(after_index);
    for (auto &col : after_tab.cols) {
        col.index = std::any_of(after_tab.indexes.begin(), after_tab.indexes.end(), [&](const IndexMeta &index) {
            return std::any_of(index.cols.begin(), index.cols.end(),
                               [&](const ColMeta &index_col) { return index_col.name == col.name; });
        });
    }
    txn_id_t txn_id = context == nullptr || context->txn_ == nullptr
                          ? INVALID_TXN_ID
                          : context->txn_->get_transaction_id();
    std::string tombstone = ddl_tombstone(txn_id, ix_name);
    append_ddl_log(context, DROP_INDEX, ix_name, tombstone, before, after);

    auto handle = ihs_.find(ix_name);
    if (handle != ihs_.end()) {
        ix_manager_->close_index(handle->second.get());
        ihs_.erase(handle);
    }
    if (disk_manager_->is_file(ix_name)) {
        disk_manager_->rename_file(ix_name, tombstone);
    }
    db_ = after;
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (const auto &col : cols) {
        col_names.push_back(col.name);
    }
    drop_index(tab_name, col_names, context);
}

void SmManager::rebuild_indexes() {
    for (auto &tab_entry : db_.tabs_) {
        TabMeta &tab = tab_entry.second;
        RmFileHandle *fh = fhs_.at(tab.name).get();
        for (const auto &index : tab.indexes) {
            std::string ix_name = ix_manager_->get_index_name(tab.name, index.cols);
            auto old = ihs_.find(ix_name);
            if (old != ihs_.end()) {
                ix_manager_->close_index(old->second.get());
                ihs_.erase(old);
            }
            if (ix_manager_->exists(tab.name, index.cols)) {
                ix_manager_->destroy_index(tab.name, index.cols);
            }
            ix_manager_->create_index(tab.name, index.cols);
            auto ih = ix_manager_->open_index(tab.name, index.cols);
            for (RmScan scan(fh); !scan.is_end(); scan.next()) {
                Rid rid = scan.rid();
                auto rec = fh->get_record(rid, nullptr);
                std::vector<char> key(index.col_tot_len);
                int offset = 0;
                for (const auto &col : index.cols) {
                    memcpy(key.data() + offset, rec->data + col.offset, col.len);
                    offset += col.len;
                }
                ih->insert_entry(key.data(), rid, nullptr);
            }
            ihs_.emplace(ix_name, std::move(ih));
        }
    }
}

void SmManager::flush_recovery_state() {
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();

    for (auto &tab_entry : db_.tabs_) {
        TabMeta &tab = tab_entry.second;
        fhs_.emplace(tab.name, rm_manager_->open_file(tab.name));
        for (const auto &index : tab.indexes) {
            std::string name = ix_manager_->get_index_name(tab.name, index.cols);
            ihs_.emplace(name, ix_manager_->open_index(tab.name, index.cols));
        }
    }
    flush_meta();
}

void SmManager::finalize_ddl(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || log_manager == nullptr) return;
    lsn_t lsn = txn->get_prev_lsn();
    while (lsn != INVALID_LSN) {
        auto record = log_manager->read_record(lsn);
        if (record == nullptr || record->log_tid_ != txn->get_transaction_id()) break;
        if (record->log_type_ == DROP_INDEX) {
            if (disk_manager_->is_file(record->aux_name_)) {
                disk_manager_->destroy_file(record->aux_name_);
            }
        } else if (record->log_type_ == DROP_TABLE) {
            std::string text(record->before_image_.begin(), record->before_image_.end());
            std::istringstream input(text);
            DbMeta before;
            input >> before;
            const TabMeta &tab = before.get_table(record->object_name_);
            std::string table_tombstone = record->aux_name_ + record->object_name_;
            if (disk_manager_->is_file(table_tombstone)) {
                disk_manager_->destroy_file(table_tombstone);
            }
            for (const auto &index : tab.indexes) {
                std::string name = ix_manager_->get_index_name(tab.name, index.cols);
                std::string tombstone = record->aux_name_ + name;
                if (disk_manager_->is_file(tombstone)) {
                    disk_manager_->destroy_file(tombstone);
                }
            }
        }
        lsn = record->prev_lsn_;
    }
}

void SmManager::rollback_ddl(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || log_manager == nullptr) return;
    std::lock_guard<std::mutex> schema_guard(schema_latch_);
    lsn_t lsn = txn->get_prev_lsn();
    while (lsn != INVALID_LSN) {
        auto record = log_manager->read_record(lsn);
        if (record == nullptr || record->log_tid_ != txn->get_transaction_id()) break;
        if (record->log_type_ >= CREATE_TABLE && record->log_type_ <= DROP_INDEX) {
            std::string text(record->before_image_.begin(), record->before_image_.end());
            std::istringstream input(text);
            DbMeta before;
            input >> before;

            if (record->log_type_ == CREATE_TABLE) {
                auto fh = fhs_.find(record->object_name_);
                if (fh != fhs_.end()) {
                    rm_manager_->close_file(fh->second.get());
                    fhs_.erase(fh);
                }
                if (disk_manager_->is_file(record->object_name_)) {
                    disk_manager_->destroy_file(record->object_name_);
                }
            } else if (record->log_type_ == CREATE_INDEX) {
                auto ih = ihs_.find(record->object_name_);
                if (ih != ihs_.end()) {
                    ix_manager_->close_index(ih->second.get());
                    ihs_.erase(ih);
                }
                if (disk_manager_->is_file(record->object_name_)) {
                    disk_manager_->destroy_file(record->object_name_);
                }
            } else if (record->log_type_ == DROP_INDEX) {
                if (!disk_manager_->is_file(record->object_name_) &&
                    disk_manager_->is_file(record->aux_name_)) {
                    disk_manager_->rename_file(record->aux_name_, record->object_name_);
                }
            } else if (record->log_type_ == DROP_TABLE) {
                const TabMeta &tab = before.get_table(record->object_name_);
                std::string table_tombstone = record->aux_name_ + record->object_name_;
                if (!disk_manager_->is_file(record->object_name_) &&
                    disk_manager_->is_file(table_tombstone)) {
                    disk_manager_->rename_file(table_tombstone, record->object_name_);
                }
                for (const auto &index : tab.indexes) {
                    std::string name = ix_manager_->get_index_name(tab.name, index.cols);
                    std::string tombstone = record->aux_name_ + name;
                    if (!disk_manager_->is_file(name) && disk_manager_->is_file(tombstone)) {
                        disk_manager_->rename_file(tombstone, name);
                    }
                }
            }
            db_ = before;
            flush_meta();

            if (record->log_type_ == DROP_TABLE) {
                const TabMeta &tab = db_.get_table(record->object_name_);
                if (fhs_.count(tab.name) == 0) {
                    fhs_.emplace(tab.name, rm_manager_->open_file(tab.name));
                }
                for (const auto &index : tab.indexes) {
                    std::string name = ix_manager_->get_index_name(tab.name, index.cols);
                    if (ihs_.count(name) == 0) {
                        ihs_.emplace(name, ix_manager_->open_index(tab.name, index.cols));
                    }
                }
            } else if (record->log_type_ == DROP_INDEX) {
                const IndexMeta *index = nullptr;
                for (const auto &tab_entry : db_.tables()) {
                    for (const auto &candidate : tab_entry.second.indexes) {
                        if (ix_manager_->get_index_name(tab_entry.first, candidate.cols) ==
                            record->object_name_) {
                            index = &candidate;
                            break;
                        }
                    }
                    if (index != nullptr) break;
                }
                if (index != nullptr && ihs_.count(record->object_name_) == 0) {
                    ihs_.emplace(record->object_name_,
                                 ix_manager_->open_index(index->tab_name, index->cols));
                }
            }
        }
        lsn = record->prev_lsn_;
    }
}
