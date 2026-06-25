/* Copyright (c) 2023 Renmin University of China */
#pragma once

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

class RecoveryManager {
   public:
    RecoveryManager(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager,
                    SmManager *sm_manager, LogManager *log_manager)
        : disk_manager_(disk_manager),
          buffer_pool_manager_(buffer_pool_manager),
          sm_manager_(sm_manager),
          log_manager_(log_manager) {}

    void analyze();
    void recover_ddl();
    void redo();
    void undo();

   private:
    void redo_record(const LogRecord &record);
    void undo_record(const LogRecord &record);

    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    SmManager *sm_manager_;
    LogManager *log_manager_;
    std::vector<std::unique_ptr<LogRecord>> records_;
    std::unordered_map<txn_id_t, lsn_t> active_txns_;
    std::unordered_set<txn_id_t> committed_txns_;
};
