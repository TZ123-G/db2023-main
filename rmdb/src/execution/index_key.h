#pragma once

#include <cstring>
#include <vector>

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

