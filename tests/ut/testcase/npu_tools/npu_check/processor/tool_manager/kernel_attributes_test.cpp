/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "tool_manager/kernel_attributes.h"

#include "injection/injection_hook.h"

#include <gtest/gtest.h>

using FunctionAttributeGetter = aclError (*)(aclrtFuncHandle, aclrtFuncAttribute, int64_t*);

FunctionAttributeGetter g_originalFunctionAttributeGetter = nullptr;
aclrtApiId g_requestedRuntimeApi = ACL_RT_API_MAX;
aclrtFuncHandle g_queriedFunction = nullptr;
uint32_t g_attributeQueryCalls = 0;

extern "C" void* acltoolGetOriginalRuntimeApi(aclrtApiId apiId)
{
    g_requestedRuntimeApi = apiId;
    return reinterpret_cast<void*>(g_originalFunctionAttributeGetter);
}

namespace npucheck {
namespace {

aclError GetAllAttributes(aclrtFuncHandle function, aclrtFuncAttribute attribute, int64_t* value)
{
    g_queriedFunction = function;
    ++g_attributeQueryCalls;
    switch (attribute) {
        case ACL_FUNC_ATTR_KERNEL_TYPE:
            *value = ACL_KERNEL_TYPE_MIX;
            return ACL_SUCCESS;
        case ACL_FUNC_ATTR_KERNEL_RATIO:
            *value = 0x00010002;
            return ACL_SUCCESS;
        case ACL_FUNC_ATTR_KERNEL_SCHED_MODE:
            *value = 1;
            return ACL_SUCCESS;
        default:
            return ACL_ERROR_INVALID_PARAM;
    }
}

aclError GetPartiallyAvailableAttributes(aclrtFuncHandle function, aclrtFuncAttribute attribute, int64_t* value)
{
    g_queriedFunction = function;
    ++g_attributeQueryCalls;
    switch (attribute) {
        case ACL_FUNC_ATTR_KERNEL_TYPE:
            *value = ACL_KERNEL_TYPE_AICORE;
            return ACL_SUCCESS;
        case ACL_FUNC_ATTR_KERNEL_RATIO:
            return ACL_ERROR_RT_FEATURE_NOT_SUPPORT;
        case ACL_FUNC_ATTR_KERNEL_SCHED_MODE:
            *value = 0;
            return ACL_SUCCESS;
        default:
            return ACL_ERROR_INVALID_PARAM;
    }
}

TEST(KernelAttributesTest, QueriesAndDecodesAllAttributes)
{
    const aclrtFuncHandle function = reinterpret_cast<aclrtFuncHandle>(0x12345678U);
    g_originalFunctionAttributeGetter = GetAllAttributes;
    g_requestedRuntimeApi = ACL_RT_API_MAX;
    g_queriedFunction = nullptr;
    g_attributeQueryCalls = 0;
    const KernelAttributes attributes = QueryKernelAttributes(function);

    EXPECT_EQ(g_requestedRuntimeApi, ACL_RT_API_aclrtGetFunctionAttribute);
    EXPECT_EQ(g_queriedFunction, function);
    EXPECT_EQ(g_attributeQueryCalls, 3);
    EXPECT_EQ(attributes.kernelType, ACL_KERNEL_TYPE_MIX);
    EXPECT_EQ(attributes.kernelTypeStatus, ACL_SUCCESS);
    EXPECT_EQ(attributes.aicRatio, 1);
    EXPECT_EQ(attributes.aivRatio, 2);
    EXPECT_EQ(attributes.kernelRatioStatus, ACL_SUCCESS);
    EXPECT_EQ(attributes.kernelSchedMode, 1);
    EXPECT_EQ(attributes.kernelSchedModeStatus, ACL_SUCCESS);
}

TEST(KernelAttributesTest, KeepsIndependentQueryStatuses)
{
    const aclrtFuncHandle function = reinterpret_cast<aclrtFuncHandle>(0x87654321U);
    g_originalFunctionAttributeGetter = GetPartiallyAvailableAttributes;
    g_requestedRuntimeApi = ACL_RT_API_MAX;
    g_queriedFunction = nullptr;
    g_attributeQueryCalls = 0;
    const KernelAttributes attributes = QueryKernelAttributes(function);

    EXPECT_EQ(g_requestedRuntimeApi, ACL_RT_API_aclrtGetFunctionAttribute);
    EXPECT_EQ(g_queriedFunction, function);
    EXPECT_EQ(g_attributeQueryCalls, 3);
    EXPECT_EQ(attributes.kernelType, ACL_KERNEL_TYPE_AICORE);
    EXPECT_EQ(attributes.kernelTypeStatus, ACL_SUCCESS);
    EXPECT_EQ(attributes.aicRatio, 0);
    EXPECT_EQ(attributes.aivRatio, 0);
    EXPECT_EQ(attributes.kernelRatioStatus, ACL_ERROR_RT_FEATURE_NOT_SUPPORT);
    EXPECT_EQ(attributes.kernelSchedMode, 0);
    EXPECT_EQ(attributes.kernelSchedModeStatus, ACL_SUCCESS);
}

TEST(KernelAttributesTest, ReportsMissingOriginalApiWithoutQuerying)
{
    g_originalFunctionAttributeGetter = nullptr;
    g_requestedRuntimeApi = ACL_RT_API_MAX;
    g_queriedFunction = nullptr;
    g_attributeQueryCalls = 0;
    const KernelAttributes attributes = QueryKernelAttributes(nullptr);

    EXPECT_EQ(g_requestedRuntimeApi, ACL_RT_API_aclrtGetFunctionAttribute);
    EXPECT_EQ(g_queriedFunction, nullptr);
    EXPECT_EQ(g_attributeQueryCalls, 0);
    EXPECT_EQ(attributes.kernelTypeStatus, ACL_ERROR_UNINITIALIZE);
    EXPECT_EQ(attributes.kernelRatioStatus, ACL_ERROR_UNINITIALIZE);
    EXPECT_EQ(attributes.kernelSchedModeStatus, ACL_ERROR_UNINITIALIZE);
}

} // namespace
} // namespace npucheck
