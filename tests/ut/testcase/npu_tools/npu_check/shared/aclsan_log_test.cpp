// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include <gtest/gtest.h>

#include "npu_tool_log.h"
#include "plog_test_library.h"

#include <cassert>
#include <cstdarg>
#include <cstring>
#include <string>
#include <thread>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

int32_t g_checkCalls = 0;
int32_t g_recordCalls = 0;
int32_t g_checkModuleId = 0;
int32_t g_recordModuleId = 0;
int32_t g_level = -1;
bool g_enabled = true;
char g_message[2048] = {};

int32_t CheckLogLevelStub(int32_t moduleId, int32_t level)
{
    ++g_checkCalls;
    g_checkModuleId = moduleId;
    g_level = level;
    return g_enabled ? 1 : 0;
}

void DlogRecordStub(int32_t moduleId, int32_t level, const char* format, ...)
{
    ++g_recordCalls;
    g_recordModuleId = moduleId;
    g_level = level;
    va_list arguments;
    va_start(arguments, format);
    const char* message = va_arg(arguments, const char*);
    va_end(arguments);
    assert(std::strcmp(format, "%s") == 0);
    assert(message != nullptr);
    (void)std::strncpy(g_message, message, sizeof(g_message) - 1U);
}

void ResetCapture()
{
    g_checkCalls = 0;
    g_recordCalls = 0;
    g_checkModuleId = 0;
    g_recordModuleId = 0;
    g_level = -1;
    std::memset(g_message, 0, sizeof(g_message));
}

} // namespace

static int RunSuiteMain()
{
    plog_test::SetApi({CheckLogLevelStub, DlogRecordStub});
    ResetCapture();
    const auto expectedLine = __LINE__ + 1;
    ASCTOOL_INFO("value=%d text=%s", 42, "100%");
    assert(g_recordCalls == 1);
    assert(g_level == DLOG_INFO);
    assert(g_recordModuleId == static_cast<int32_t>(ASCTOOL));
    const auto location = std::string(__FILE__) + ":" + std::to_string(expectedLine) + "][RunSuiteMain]";
    assert(std::strstr(g_message, location.c_str()) != nullptr);
    assert(std::strstr(g_message, "value=42 text=100%\n") != nullptr);

    ResetCapture();
    ASCTOOL_DEBUG("debug");
    assert(g_recordCalls == 1 && g_level == DLOG_DEBUG);
    ASCTOOL_WARNING("warning");
    assert(g_recordCalls == 2 && g_level == DLOG_WARN);
    ASCTOOL_ERROR("error");
    assert(g_recordCalls == 3 && g_level == DLOG_ERROR);

    std::thread worker([] {
        ASCTOOL_INFO("worker");
        assert(std::strstr(g_message, std::to_string(syscall(SYS_gettid)).c_str()) != nullptr);
        assert(syscall(SYS_gettid) != getpid());
    });
    worker.join();

    ResetCapture();
    g_enabled = false;
    int evaluated = 0;
    ASCTOOL_DEBUG("filtered %d", ++evaluated);
    ASCTOOL_INFO("filtered %d", ++evaluated);
    ASCTOOL_WARNING("filtered %d", ++evaluated);
    assert(g_recordCalls == 0);
    assert(evaluated == 0);
    g_enabled = true;

    ResetCapture();
    ASCTOOL_ERROR("%s", (std::string(4096, 'x') + "\ncompiler-error-tail").c_str());
    assert(g_recordCalls == 1); // Record size handling belongs to dlog.
    plog_test::ResetApi();
    return 0;
}

TEST(AclsanPlog, Main) { ASSERT_EQ(RunSuiteMain(), 0) << "AclsanPlog reported failure"; }
