/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

/**
 * @brief 辅助：将 LockMode 转换为 GroupLockMode
 */
LockManager::GroupLockMode LockManager::LockModeToGroup(LockMode mode) {
    switch (mode) {
        case LockMode::SHARED:               return GroupLockMode::S;
        case LockMode::EXLUCSIVE:            return GroupLockMode::X;
        case LockMode::INTENTION_SHARED:     return GroupLockMode::IS;
        case LockMode::INTENTION_EXCLUSIVE:  return GroupLockMode::IX;
        case LockMode::S_IX:                 return GroupLockMode::SIX;
    }
    return GroupLockMode::NON_LOCK;
}

/**
 * @brief 辅助：合并事务已有的锁模式与新请求的锁模式，返回组合后的 GroupLockMode
 *
 * 用于计算一个事务在单个资源上的"总效果"
 * IS+S → S, S+IX → SIX, IS+IX → IX 等
 */
LockManager::GroupLockMode LockManager::MergeGroupMode(GroupLockMode existing, LockMode additional) {
    GroupLockMode add_grp = LockModeToGroup(additional);

    if (existing == GroupLockMode::NON_LOCK) return add_grp;

    switch (existing) {
        case GroupLockMode::IS:
            switch (additional) {
                case LockMode::INTENTION_SHARED:     return GroupLockMode::IS;   // IS+IS
                case LockMode::SHARED:               return GroupLockMode::S;    // IS+S
                case LockMode::INTENTION_EXCLUSIVE:  return GroupLockMode::IX;   // IS+IX (no S component)
                case LockMode::S_IX:                 return GroupLockMode::SIX;  // IS+SIX
                case LockMode::EXLUCSIVE:            return GroupLockMode::X;    // IS+X
            }
            break;
        case GroupLockMode::IX:
            switch (additional) {
                case LockMode::INTENTION_SHARED:     return GroupLockMode::IX;   // IX+IS
                case LockMode::SHARED:               return GroupLockMode::SIX;  // IX+S → SIX
                case LockMode::INTENTION_EXCLUSIVE:  return GroupLockMode::IX;   // IX+IX
                case LockMode::S_IX:                 return GroupLockMode::SIX;  // IX+SIX
                case LockMode::EXLUCSIVE:            return GroupLockMode::X;    // IX+X
            }
            break;
        case GroupLockMode::S:
            switch (additional) {
                case LockMode::INTENTION_SHARED:     return GroupLockMode::S;    // S+IS
                case LockMode::SHARED:               return GroupLockMode::S;    // S+S
                case LockMode::INTENTION_EXCLUSIVE:  return GroupLockMode::SIX;  // S+IX → SIX
                case LockMode::S_IX:                 return GroupLockMode::SIX;  // S+SIX
                case LockMode::EXLUCSIVE:            return GroupLockMode::X;    // S+X
            }
            break;
        case GroupLockMode::SIX:
            switch (additional) {
                case LockMode::INTENTION_SHARED:     return GroupLockMode::SIX;
                case LockMode::SHARED:               return GroupLockMode::SIX;
                case LockMode::INTENTION_EXCLUSIVE:  return GroupLockMode::SIX;
                case LockMode::S_IX:                 return GroupLockMode::SIX;
                case LockMode::EXLUCSIVE:            return GroupLockMode::X;
            }
            break;
        case GroupLockMode::X:
            return GroupLockMode::X;   // X + anything = X
        default:
            break;
    }
    return add_grp;
}

/**
 * @brief 表级锁兼容矩阵
 *
 *         IS   IX   S    SIX  X
 *   IS    ✓    ✓    ✓    ✓    ✗
 *   IX    ✓    ✓    ✗    ✗    ✗
 *   S     ✓    ✗    ✓    ✗    ✗
 *   SIX   ✓    ✗    ✗    ✗    ✗
 *   X     ✗    ✗    ✗    ✗    ✗
 */
bool LockManager::TableLockModeCompatible(GroupLockMode a, GroupLockMode b) {
    static const bool compat[6][6] = {
        // NON_LOCK  IS      IX      S       X       SIX
        {  true,     true,   true,   true,   true,   true   },  // NON_LOCK
        {  true,     true,   true,   true,   false,  true   },  // IS
        {  true,     true,   true,   false,  false,  false  },  // IX
        {  true,     true,   false,  true,   false,  false  },  // S
        {  true,     false,  false,  false,  false,  false  },  // X
        {  true,     true,   false,  false,  false,  false  },  // SIX
    };
    return compat[static_cast<int>(a)][static_cast<int>(b)];
}

/**
 * @brief 记录级锁兼容矩阵：S↔S ✓，其余 ✗
 */
bool LockManager::RecordLockModeCompatible(LockMode a, LockMode b) {
    if (a == LockMode::SHARED && b == LockMode::SHARED) return true;
    return false;  // EXLUCSIVE conflicts with everything
}

/**
 * @brief 计算某事务在当前队列上的有效锁模式（已有请求的合并结果）
 */
LockManager::GroupLockMode LockManager::GetTxnEffectiveMode(LockRequestQueue& queue, txn_id_t txn_id) {
    GroupLockMode mode = GroupLockMode::NON_LOCK;
    for (auto& req : queue.request_queue_) {
        if (req.granted_ && req.txn_id_ == txn_id) {
            mode = MergeGroupMode(mode, req.lock_mode_);
        }
    }
    return mode;
}

/**
 * @brief 重新计算整个队列的 GroupLockMode（作为优化缓存）
 */
LockManager::GroupLockMode LockManager::RecomputeGroupLockMode(LockRequestQueue& queue) {
    if (queue.request_queue_.empty()) return GroupLockMode::NON_LOCK;
    GroupLockMode mode = GroupLockMode::NON_LOCK;
    for (auto& req : queue.request_queue_) {
        if (req.granted_) {
            GroupLockMode m = MergeGroupMode(mode, req.lock_mode_);
            if (static_cast<int>(m) > static_cast<int>(mode)) mode = m;
        }
    }
    return mode;
}

/**
 * @brief 统一的表级锁请求处理核心（不等待，不阻塞）
 */
void LockManager::RequestTableLock(Transaction* txn, const LockDataId& lock_data_id, LockMode requested_mode) {
    txn_id_t txn_id = txn->get_transaction_id();

    // 1. 检查事务状态
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn_id, AbortReason::LOCK_ON_SHIRINKING);
    }

    auto& queue = lock_table_[lock_data_id];

    // 2. 幂等检查：如果同一事务已持有完全相同的锁模式，直接返回
    for (auto& req : queue.request_queue_) {
        if (req.txn_id_ == txn_id && req.granted_ && req.lock_mode_ == requested_mode) {
            return;
        }
    }

    // 3. 计算当前事务已有模式的合并效果
    GroupLockMode current_effective = GetTxnEffectiveMode(queue, txn_id);

    // 4. 计算加入新请求后的总效果
    GroupLockMode new_effective = MergeGroupMode(current_effective, requested_mode);

    // 5. 收集其他事务的有效锁模式，逐一检查兼容性
    std::unordered_map<txn_id_t, GroupLockMode> other_effective;
    for (auto& req : queue.request_queue_) {
        if (req.granted_ && req.txn_id_ != txn_id) {
            other_effective[req.txn_id_] = MergeGroupMode(other_effective[req.txn_id_], req.lock_mode_);
        }
    }
    for (auto& [other_id, other_mode] : other_effective) {
        (void)other_id;
        if (!TableLockModeCompatible(other_mode, new_effective)) {
            throw TransactionAbortException(txn_id, AbortReason::DEADLOCK_PREVENTION);
        }
    }

    // 6. 通过：添加锁请求并立即授予
    queue.request_queue_.push_back(LockRequest(txn_id, requested_mode));
    queue.request_queue_.back().granted_ = true;
    queue.group_lock_mode_ = RecomputeGroupLockMode(queue);

    // 7. 添加到事务的 lock_set
    txn->get_lock_set()->insert(lock_data_id);
}

/**
 * @brief 统一的记录级锁请求处理核心（不等待，不阻塞）
 */
void LockManager::RequestRecordLock(Transaction* txn, const LockDataId& lock_data_id, LockMode requested_mode) {
    txn_id_t txn_id = txn->get_transaction_id();

    // 1. 检查事务状态
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn_id, AbortReason::LOCK_ON_SHIRINKING);
    }

    auto& queue = lock_table_[lock_data_id];

    // 2. 幂等检查：如果同一事务已持有完全相同的锁，直接返回
    for (auto& req : queue.request_queue_) {
        if (req.txn_id_ == txn_id && req.granted_ && req.lock_mode_ == requested_mode) {
            return;
        }
    }

    // 3. 如果该事务已有 S 锁，且新请求是 X → 尝试升级
    bool need_upgrade = false;
    for (auto& req : queue.request_queue_) {
        if (req.txn_id_ == txn_id && req.granted_ && req.lock_mode_ == LockMode::SHARED && requested_mode == LockMode::EXLUCSIVE) {
            need_upgrade = true;
            break;
        }
    }

    // 4. 检查与其他事务的兼容性
    for (auto& req : queue.request_queue_) {
        if (req.granted_ && req.txn_id_ != txn_id) {
            if (!RecordLockModeCompatible(req.lock_mode_, requested_mode)) {
                throw TransactionAbortException(txn_id, AbortReason::DEADLOCK_PREVENTION);
            }
        }
    }

    // 5. 处理升级
    if (need_upgrade) {
        for (auto& req : queue.request_queue_) {
            if (req.txn_id_ == txn_id && req.granted_ && req.lock_mode_ == LockMode::SHARED) {
                // 从 S 升级到 X：修改原请求的锁模式
                req.lock_mode_ = LockMode::EXLUCSIVE;
                break;
            }
        }
        queue.group_lock_mode_ = RecomputeGroupLockMode(queue);
        txn->get_lock_set()->insert(lock_data_id);
        return;
    }

    // 6. 通过：添加新请求
    queue.request_queue_.push_back(LockRequest(txn_id, requested_mode));
    queue.request_queue_.back().granted_ = true;
    queue.group_lock_mode_ = RecomputeGroupLockMode(queue);

    txn->get_lock_set()->insert(lock_data_id);
}

// ===================== 公有接口 =====================

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    std::lock_guard<std::mutex> guard(latch_);
    RequestRecordLock(txn, lock_data_id, LockMode::SHARED);
    return true;
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    std::lock_guard<std::mutex> guard(latch_);
    RequestRecordLock(txn, lock_data_id, LockMode::EXLUCSIVE);
    return true;
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    std::lock_guard<std::mutex> guard(latch_);
    RequestTableLock(txn, lock_data_id, LockMode::SHARED);
    return true;
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    std::lock_guard<std::mutex> guard(latch_);
    RequestTableLock(txn, lock_data_id, LockMode::EXLUCSIVE);
    return true;
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    std::lock_guard<std::mutex> guard(latch_);
    RequestTableLock(txn, lock_data_id, LockMode::INTENTION_SHARED);
    return true;
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    std::lock_guard<std::mutex> guard(latch_);
    RequestTableLock(txn, lock_data_id, LockMode::INTENTION_EXCLUSIVE);
    return true;
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    txn_id_t txn_id = txn->get_transaction_id();

    std::lock_guard<std::mutex> guard(latch_);

    auto it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) return false;

    auto& queue = it->second;
    auto& req_list = queue.request_queue_;

    // 移除该事务的所有锁请求
    for (auto req_it = req_list.begin(); req_it != req_list.end(); ) {
        if (req_it->txn_id_ == txn_id) {
            req_it = req_list.erase(req_it);
        } else {
            ++req_it;
        }
    }

    // 重新计算队列的 group_lock_mode
    queue.group_lock_mode_ = RecomputeGroupLockMode(queue);

    // 从事务的 lock_set 中移除
    txn->get_lock_set()->erase(lock_data_id);

    // 首次解锁 → 进入 SHRINKING 阶段
    if (txn->get_state() == TransactionState::GROWING) {
        txn->set_state(TransactionState::SHRINKING);
    }

    // 如果队列已空，清理锁表条目
    if (queue.request_queue_.empty()) {
        lock_table_.erase(it);
    }

    return true;
}
