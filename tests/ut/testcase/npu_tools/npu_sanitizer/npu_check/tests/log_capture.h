// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#ifndef NPU_CHECK_TEST_LOG_CAPTURE_H
#define NPU_CHECK_TEST_LOG_CAPTURE_H

#include "dlog_pub.h"
#include "plog_test_library.h"
#include <string>

namespace npucheck::test {
using LogSink = void (*)(void*, int32_t, const char*);

// Capture the dlog API boundary without changing product logging or writing to a terminal.
class ScopedLogCapture {
public:
    explicit ScopedLogCapture(std::string& output);
    ScopedLogCapture(LogSink sink, void* context, int32_t minimumLevel = DLOG_DEBUG) noexcept;
    ~ScopedLogCapture();
    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

private:
    LogSink previousSink_;
    void* previousContext_;
    int32_t previousLevel_;
    plog_test::Api previousApi_;
};
} // namespace npucheck::test
#endif
