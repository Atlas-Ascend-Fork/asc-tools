// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#ifndef NPU_SANITIZER_TEST_PLOG_CAPTURE_H
#define NPU_SANITIZER_TEST_PLOG_CAPTURE_H

#include "npu_tool_log.h"
#include "plog_test_library.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

// Capture the real shared sink at the CANN boundary, without redirecting application output.
class PlogCapture {
public:
    PlogCapture()
    {
        active_ = this;
        plog_test::SetApi({Check, Record});
    }
    ~PlogCapture()
    {
        plog_test::ResetApi();
        active_ = nullptr;
    }
    PlogCapture(const PlogCapture&) = delete;
    PlogCapture& operator=(const PlogCapture&) = delete;
    std::string Text() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }

private:
    static int32_t Check(int32_t, int32_t) { return 1; }
    static void Record(int32_t, int32_t, const char* format, ...)
    {
        char message[2048] = {};
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        std::lock_guard<std::mutex> lock(active_->mutex_);
        active_->text_ += message;
        active_->text_ += '\n';
    }
    inline static PlogCapture* active_ = nullptr;
    mutable std::mutex mutex_;
    std::string text_;
};
#endif
