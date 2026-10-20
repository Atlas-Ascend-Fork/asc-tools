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

#include "aclpti/aclpti.h"
#include "injection/runtime_stub_api.h"
#include "profiling/prof_api.h"

// 最小可跑桩（外部链接，与其他单测用例的 mock 形式一致）：本用例只做空参校验，
// 这里仅补齐静态链接的传递引用；场景化行为由其他用例内的 mock 各自还原。
std::int32_t MsprofStart(std::uint32_t, const void*, std::uint32_t) { return 0; }

std::int32_t MsprofStop(std::uint32_t, const void*, std::uint32_t) { return 0; }

std::int32_t MsprofRegisterDataCallback(std::uint32_t, void*) { return 0; }

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

int DummyMalloc(void** pointer, std::size_t size, aclrtMemMallocPolicy)
{
    *pointer = std::malloc(size);
    return *pointer == nullptr ? -1 : 0;
}

int DummyFree(void* pointer)
{
    std::free(pointer);
    return 0;
}

int DummyMemcpy(void*, std::size_t, const void*, std::size_t, aclrtMemcpyKind) { return 0; }
int DummyMemset(void*, std::size_t, int, std::size_t) { return 0; }
int DummyLaunch(void*, uint32_t, const void*, std::size_t, void*) { return 0; }
int DummySynchronize(void*) { return 0; }

} // namespace

TEST(NpuComputeInitializeFailure, RejectsSubscriberAfterInitializationFailure)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_TRUE(setenv("NPU_COMPUTE_TEST_PTI_INITIALIZE_FAILURE", "1", 1) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMalloc", &DummyMalloc) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtFree", &DummyFree) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemcpy", &DummyMemcpy) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemset", &DummyMemset) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtLaunchKernel", &DummyLaunch) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &DummySynchronize) == 0);

    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&subscriber, nullptr, nullptr, nullptr) == ACLPTI_ERROR_INTERNAL);
    ASSERT_TRUE(subscriber == nullptr);
}
