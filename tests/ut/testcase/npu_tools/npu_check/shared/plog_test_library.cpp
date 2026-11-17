// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "plog_test_library.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>
namespace {
std::mutex apiMutex;
plog_test::Api api{};
plog_test::Api GetApi()
{
    std::lock_guard<std::mutex> lock(apiMutex);
    return api;
}
} // namespace
namespace plog_test {
Api SetApi(Api value)
{
    std::lock_guard<std::mutex> lock(apiMutex);
    const Api previous = api;
    api = value;
    return previous;
}
void ResetApi() { SetApi({nullptr, nullptr}); }
} // namespace plog_test
extern "C" int32_t CheckLogLevel(int32_t moduleId, int32_t level)
{
    const auto current = GetApi();
    return current.check == nullptr ? 0 : current.check(moduleId, level);
}
extern "C" void DlogRecord(int32_t moduleId, int32_t level, const char* format, ...)
{
    const auto current = GetApi();
    if (current.record == nullptr) {
        return;
    }
    char buffer[2048] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    current.record(moduleId, level, "%s", buffer);
}
