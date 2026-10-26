/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>
#include "npu_compute_log.h"
#include "plog_test_library.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
struct Record {
    int32_t module;
    int32_t level;
    std::string message;
};
int32_t threshold = DLOG_ERROR;
std::vector<Record> records;
int32_t CheckLevel(int32_t module, int32_t level) { return module == ASCTOOL_MODULE_ID && level >= threshold ? 1 : 0; }
void CaptureRecord(int32_t module, int32_t level, const char* format, ...)
{
    char text[4096]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    records.push_back({module, level, text});
}
class NpuComputePlog : public testing::Test {
protected:
    void SetUp() override
    {
        records.clear();
        threshold = DLOG_ERROR;
        plog_test::SetApi({CheckLevel, CaptureRecord});
    }
    void TearDown() override { plog_test::ResetApi(); }
};
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const char* value) : name_(name)
    {
        const char* old = std::getenv(name);
        present_ = old != nullptr;
        if (present_)
            old_ = old;
        setenv(name, value, 1);
    }
    ~ScopedEnvironment()
    {
        if (present_)
            setenv(name_.c_str(), old_.c_str(), 1);
        else
            unsetenv(name_.c_str());
    }

private:
    std::string name_, old_;
    bool present_ = false;
};
TEST_F(NpuComputePlog, BackendThresholdControlsAllSeverities)
{
    int evaluated = 0;
    ASCTOOL_DEBUG("[aclpti] debug=%d", ++evaluated);
    ASCTOOL_INFO("[npu-compute] info=%d", ++evaluated);
    ASCTOOL_WARNING("[tool_injection] warning=%d", ++evaluated);
    EXPECT_EQ(evaluated, 0);
    EXPECT_TRUE(records.empty());
    ASCTOOL_ERROR("[npu-compute] failed: code=%d", 17);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].level, DLOG_ERROR);
    EXPECT_EQ(records[0].module, ASCTOOL_MODULE_ID);
    EXPECT_NE(records[0].message.find("failed: code=17"), std::string::npos);

    threshold = DLOG_DEBUG;
    ASCTOOL_DEBUG("[aclpti] debug");
    ASCTOOL_INFO("[npu-compute] info");
    ASCTOOL_WARNING("[tool_injection] warning");
    ASSERT_EQ(records.size(), 4U);
    EXPECT_EQ(records[1].level, DLOG_DEBUG);
    EXPECT_EQ(records[2].level, DLOG_INFO);
    EXPECT_EQ(records[3].level, DLOG_WARN);
}
TEST_F(NpuComputePlog, LegacyEnvironmentDoesNotOverrideBackend)
{
    {
        ScopedEnvironment compute("NPU_COMPUTE_DEBUG", "1");
        ScopedEnvironment injection("ACL_TOOL_INJECTION_DEBUG", "1");
        EXPECT_FALSE(ASCTOOL_DEBUG_ENABLED());
        ASCTOOL_DEBUG("[aclpti] suppressed");
        EXPECT_TRUE(records.empty());
    }
    {
        ScopedEnvironment compute("NPU_COMPUTE_DEBUG", "0");
        ScopedEnvironment injection("ACL_TOOL_INJECTION_DEBUG", "0");
        threshold = DLOG_DEBUG;
        EXPECT_TRUE(ASCTOOL_DEBUG_ENABLED());
        ASCTOOL_DEBUG("[aclpti] enabled");
        ASSERT_EQ(records.size(), 1U);
    }
}
TEST_F(NpuComputePlog, RecordsSourceMetadataAndLiteralPercentWithoutStderr)
{
    testing::internal::CaptureStderr();
    const int line = __LINE__ + 1;
    ASCTOOL_ERROR("[npu-compute] message=%s", "100% literal %s");
    const std::string stderrText = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(stderrText.empty());
    ASSERT_EQ(records.size(), 1U);
    EXPECT_NE(records[0].message.find(std::string(__FILE__) + ":" + std::to_string(line)), std::string::npos);
    EXPECT_NE(records[0].message.find("[TestBody]"), std::string::npos);
    EXPECT_NE(records[0].message.find("[npu-compute] message=100% literal %s"), std::string::npos);
}
} // namespace
