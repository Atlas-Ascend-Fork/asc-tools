// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include <gtest/gtest.h>

#include "acl_san/aclsan_api.h"
#include "npu_tool_log.h"
#include "plog_test_library.h"
#include "acl/acl_base.h"

#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

int32_t g_plogRecordCount = 0;
int32_t g_plogLevel = -1;
char g_plogMessage[2048] = {};

int32_t CheckLogLevelStub(int32_t, int32_t) { return 1; }

void DlogRecordStub(int32_t, int32_t level, const char* format, ...)
{
    ++g_plogRecordCount;
    g_plogLevel = level;
    va_list arguments;
    va_start(arguments, format);
    const char* message = va_arg(arguments, const char*);
    va_end(arguments);
    assert(std::strcmp(format, "%s") == 0);
    assert(message != nullptr);
    (void)std::strncpy(g_plogMessage, message, sizeof(g_plogMessage) - 1U);
}

AclsanStatus ValidatePointer(const void* value)
{
    ACLSAN_CHECK_NULLPTR("TestApi", value);
    return ACLSAN_STATUS_SUCCESS;
}

aclError CheckAclResult(aclError status, int& calls, int& messages, bool& continued)
{
    ACLSAN_RETURN_IF_ACL_ERROR((++calls, status), (++messages, "Parameter query 100% failed"));
    continued = true;
    return ACL_SUCCESS;
}

void TestAclErrorReturn()
{
    int calls = 0;
    int messages = 0;
    bool continued = false;
    const int32_t initialRecords = g_plogRecordCount;
    assert(CheckAclResult(ACL_SUCCESS, calls, messages, continued) == ACL_SUCCESS);
    assert(calls == 1 && messages == 0 && continued);
    assert(g_plogRecordCount == initialRecords);

    continued = false;
    assert(CheckAclResult(ACL_ERROR_INVALID_PARAM, calls, messages, continued) == ACL_ERROR_INVALID_PARAM);
    assert(calls == 2 && messages == 1 && !continued);
    assert(g_plogRecordCount == initialRecords + 1);
    assert(g_plogLevel == DLOG_ERROR);
    const std::string expected = "Parameter query 100% failed: result=" + std::to_string(ACL_ERROR_INVALID_PARAM);
    assert(std::strstr(g_plogMessage, expected.c_str()) != nullptr);
    assert(std::strstr(g_plogMessage, "[CheckAclResult]") != nullptr);
}

std::string CaptureNullPointerLog()
{
    std::FILE* capture = std::tmpfile();
    assert(capture != nullptr);
    const int savedStderr = dup(STDERR_FILENO);
    assert(savedStderr >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);

    assert(ValidatePointer(nullptr) == ACLSAN_STATUS_ERROR_INVALID_PARAMETER);

    assert(dup2(savedStderr, STDERR_FILENO) >= 0);
    assert(close(savedStderr) == 0);
    assert(std::fseek(capture, 0, SEEK_SET) == 0);

    std::array<char, 256> buffer{};
    const size_t bytes = std::fread(buffer.data(), 1, buffer.size() - 1, capture);
    assert(std::fclose(capture) == 0);
    return std::string(buffer.data(), bytes);
}

} // namespace

TEST(AclsanLog, Main)
{
    plog_test::SetApi({CheckLogLevelStub, DlogRecordStub});
    int value = 0;
    assert(ValidatePointer(&value) == ACLSAN_STATUS_SUCCESS);

    const std::string logs = CaptureNullPointerLog();
    assert(logs.empty()); // Internal errors belong to plog, not the check/console channel.
    assert(g_plogRecordCount == 1);
    assert(g_plogLevel == DLOG_ERROR);
    assert(std::strstr(g_plogMessage, "aclsan_log_test.cpp:") != nullptr);
    assert(std::strstr(g_plogMessage, "TestApi: value is nullptr") != nullptr);
    assert(std::strstr(g_plogMessage, "[ValidatePointer]TestApi: value is nullptr") != nullptr);
    const std::string longMessage = std::string(4096, 'x') + "\napi-error-tail";
    ASCTOOL_ERROR("%s", longMessage.c_str());
    assert(g_plogRecordCount == 2); // Long messages are passed to dlog as one record.
    TestAclErrorReturn();
    plog_test::ResetApi();
}
