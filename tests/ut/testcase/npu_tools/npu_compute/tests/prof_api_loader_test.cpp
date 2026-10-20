/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "isolated_gtest.h"

#include "injection/prof_api_stub.h"

#include <cstdio>
#include <cstdlib>

TEST(NpuComputeProfApiLoader, RetriesInitializationAfterFailure)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(unsetenv("NPU_COMPUTE_TEST_INIT_SUCCESS"), 0);
    ASSERT_EQ(unsetenv("NPU_COMPUTE_TEST_INITIALIZED"), 0);
    ASSERT_EQ(setenv("ACL_API_INJECTION", NPU_COMPUTE_LOADER_PLUGIN_PATH, 1), 0);
    ASSERT_NE(ProfApiLoadApiInjectionFromEnv(), 0);
    ASSERT_EQ(setenv("NPU_COMPUTE_TEST_INIT_SUCCESS", "1", 1), 0);
    ASSERT_EQ(ProfApiLoadApiInjectionFromEnv(), 0);
    const char* initialized = std::getenv("NPU_COMPUTE_TEST_INITIALIZED");
    ASSERT_NE(initialized, nullptr);
    EXPECT_EQ(initialized[0], '1');
}
