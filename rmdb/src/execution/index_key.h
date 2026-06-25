#pragma once

#include <cstring>
#include <vector>

#include "common/context.h"
#include "system/sm_meta.h"

inline std::vector<char> build_index_key(const IndexMeta &index, const char *record_data) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (const auto &col : index.cols) {
        memcpy(key.data() + offset, record_data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

inline lsn_t append_index_log(Context *context, LogType type, const std::string &index_name,
                              const std::vector<char> &key, const Rid &rid) {
    if (context == nullptr || context->txn_ == nullptr || context->log_mgr_ == nullptr) {
        return INVALID_LSN;
    }
    LogRecord log(type, context->txn_->get_transaction_id());
    log.set_object_name(index_name);
    log.set_key(key.data(), key.size());
    log.set_rid(rid);
    lsn_t lsn = context->log_mgr_->append(log, context->txn_->get_prev_lsn());
    context->txn_->set_prev_lsn(lsn);
    return lsn;
}
