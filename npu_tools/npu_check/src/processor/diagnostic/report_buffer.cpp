/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "diagnostic/report_buffer.h"

namespace npucheck::diagnostic {

bool ReportBuffer::Append(const std::string& text) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_ || text.empty()) {
            return !failed_;
        }
        if (buffer_.size() >= npucheck::ipc::MAX_RESULT_BYTES) {
            truncated_ = true;
            return false;
        }
        // 触及上限时截到边界为止而不是整段丢弃：报告本来就是给人读的，半条记录也比
        // 突然消失更容易定位问题，何况 truncated 标志已经把不完整这件事讲清楚了。
        const size_t room = npucheck::ipc::MAX_RESULT_BYTES - buffer_.size();
        if (text.size() > room) {
            buffer_.append(text, 0, room);
            truncated_ = true;
            return false;
        }
        buffer_.append(text);
        return true;
    } catch (...) {
        // 只可能是分配失败。此时缓冲区可能处于任意状态，标记为不可用，由调用方改发 Error。
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            failed_ = true;
        } catch (...) {
            // 加锁本身失败时无处可记，保持 noexcept 语义直接返回。
        }
        return false;
    }
}

std::string ReportBuffer::Take() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string taken;
        taken.swap(buffer_);
        return taken;
    } catch (...) {
        return {};
    }
}

bool ReportBuffer::Truncated() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return truncated_;
    } catch (...) {
        return true;
    }
}

bool ReportBuffer::Failed() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    } catch (...) {
        return true;
    }
}

size_t ReportBuffer::Size() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.size();
    } catch (...) {
        return 0;
    }
}

} // namespace npucheck::diagnostic
