/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    rid_ = Rid{RM_FIRST_RECORD_PAGE, -1};
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    int page_no = rid_.page_no;
    int slot_no = rid_.slot_no;
    if (page_no < RM_FIRST_RECORD_PAGE) {
        page_no = RM_FIRST_RECORD_PAGE;
        slot_no = -1;
    }

    for (; page_no < file_handle_->file_hdr_.num_pages; ++page_no) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);
        int next_slot = Bitmap::next_bit(true, page_handle.bitmap,
                                         file_handle_->file_hdr_.num_records_per_page, slot_no);
        file_handle_->buffer_pool_manager_->unpin_page(PageId{file_handle_->fd_, page_no}, false);
        if (next_slot < file_handle_->file_hdr_.num_records_per_page) {
            rid_ = Rid{page_no, next_slot};
            return;
        }
        slot_no = -1;
    }
    rid_ = Rid{file_handle_->file_hdr_.num_pages, 0};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}
