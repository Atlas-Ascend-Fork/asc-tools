// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "log_capture.h"
#include <cstdarg>
#include <cstdio>

namespace {
thread_local npucheck::test::LogSink g_sink = nullptr;
thread_local void* g_context = nullptr;
thread_local int32_t g_minimumLevel = DLOG_ERROR;

int32_t CheckCaptureLevel(int32_t, int32_t level) { return g_sink != nullptr && level >= g_minimumLevel ? 1 : 0; }

void CaptureRecord(int32_t, int32_t level, const char* format, ...)
{
    if (g_sink == nullptr || level < g_minimumLevel) {
        return;
    }
    char message[4096];
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (length >= 0) {
        g_sink(g_context, level, message);
    }
}
} // namespace

namespace npucheck::test {
ScopedLogCapture::ScopedLogCapture(std::string& output)
    : ScopedLogCapture(
          [](void* context, int32_t, const char* message) { static_cast<std::string*>(context)->append(message); },
          &output)
{}

ScopedLogCapture::ScopedLogCapture(LogSink sink, void* context, int32_t minimumLevel) noexcept
    : previousSink_(g_sink),
      previousContext_(g_context),
      previousLevel_(g_minimumLevel),
      previousApi_(plog_test::SetApi({CheckCaptureLevel, CaptureRecord}))
{
    g_sink = sink;
    g_context = context;
    g_minimumLevel = minimumLevel;
}

ScopedLogCapture::~ScopedLogCapture()
{
    g_sink = previousSink_;
    g_context = previousContext_;
    g_minimumLevel = previousLevel_;
    plog_test::SetApi(previousApi_);
}
} // namespace npucheck::test
