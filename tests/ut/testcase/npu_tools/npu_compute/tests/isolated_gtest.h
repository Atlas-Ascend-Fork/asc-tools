/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_ISOLATED_GTEST_H
#define NPU_COMPUTE_ISOLATED_GTEST_H

#include "test_process.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>

namespace npucompute::test {
inline std::string CurrentTestName()
{
    const auto* test = testing::UnitTest::GetInstance()->current_test_info();
    return std::string(test->test_suite_name()) + "." + test->name();
}

inline bool IsIsolatedTestChild()
{
    const char* name = std::getenv("NPU_COMPUTE_ISOLATED_GTEST");
    return name != nullptr && name == CurrentTestName();
}

inline testing::AssertionResult RunCurrentTestInChild()
{
    ProcessOptions options;
    const std::string name = CurrentTestName();
    options.arguments = {"/proc/self/exe", "--gtest_filter=" + name};
    options.environment["NPU_COMPUTE_ISOLATED_GTEST"] = name;
    const auto result = RunProcess(options);
    if (result.launchError != 0 || result.timedOut || result.signal != 0 || result.exitCode != 0) {
        return testing::AssertionFailure() << "Isolated GTest " << name << " failed: launchError=" << result.launchError
                                           << ", timedOut=" << result.timedOut << ", signal=" << result.signal
                                           << ", exitCode=" << result.exitCode << "\n"
                                           << result.standardOutput << result.standardError;
    }
    return testing::AssertionSuccess();
}
} // namespace npucompute::test

// Parent and child run the same real GTest case. Product assertions execute only in the fresh child.
#define NPU_COMPUTE_ISOLATE_TEST()                                  \
    do {                                                            \
        if (!npucompute::test::IsIsolatedTestChild()) {             \
            ASSERT_TRUE(npucompute::test::RunCurrentTestInChild()); \
            return;                                                 \
        }                                                           \
    } while (false)

#endif
