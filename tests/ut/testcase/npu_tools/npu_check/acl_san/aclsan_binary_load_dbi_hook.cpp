/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_api.h"
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"

#include <cstdio>
#include <cstring>

namespace {

#define CHECK(expression)                                                                 \
    do {                                                                                  \
        if (!(expression)) {                                                              \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expression); \
            return 1;                                                                     \
        }                                                                                 \
    } while (false)

int g_originalDataCalls = 0;
int g_sentinelDataCalls = 0;
const void* g_forwardedData = nullptr;
size_t g_forwardedLength = 0;
const aclrtBinaryLoadOptions* g_forwardedDataOptions = nullptr;

aclError OriginalDataLoad(
    const void* data, size_t length, const aclrtBinaryLoadOptions* options, aclrtBinHandle* binHandle)
{
    ++g_originalDataCalls;
    g_forwardedData = data;
    g_forwardedLength = length;
    g_forwardedDataOptions = options;
    if (binHandle != nullptr) {
        *binHandle = reinterpret_cast<aclrtBinHandle>(0x51);
    }
    return 81;
}

aclError SentinelDataLoad(const void*, size_t, const aclrtBinaryLoadOptions*, aclrtBinHandle*)
{
    ++g_sentinelDataCalls;
    return 91;
}

void Callback(void*, AclsanCallbackDomain, AclsanCallbackId, const void*) {}

} // namespace

int main()
{
    CHECK(RuntimeStubSetOriginFunction("aclrtBinaryLoadFromData", &OriginalDataLoad) == ACL_SUCCESS);

    AclsanSubscriberHandle subscriber = nullptr;
    CHECK(aclsanSubscribe(&subscriber, &Callback, nullptr) == ACLSAN_STATUS_SUCCESS);
    CHECK(acltoolRegisterAclrtBinaryLoadFromDataCallbacks(&SentinelDataLoad) == ACL_SUCCESS);
    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);

    const unsigned char binary[] = {0x7f, 'E', 'L', 'F'};
    aclrtBinaryLoadOptions dataOptions{};
    aclrtBinHandle dataHandle = nullptr;
    CHECK(aclrtBinaryLoadFromData(binary, sizeof(binary), &dataOptions, &dataHandle) == ACL_ERROR_FAILURE);
    CHECK(g_sentinelDataCalls == 0);
    CHECK(g_originalDataCalls == 0);

    CHECK(
        aclsanEnableCallback(0, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(acltoolRegisterAclrtBinaryLoadFromDataCallbacks(&SentinelDataLoad) == ACL_SUCCESS);
    CHECK(aclrtBinaryLoadFromData(binary, sizeof(binary), &dataOptions, &dataHandle) == 91);
    CHECK(g_sentinelDataCalls == 1);

    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(aclrtBinaryLoadFromData(binary, sizeof(binary), &dataOptions, &dataHandle) == ACL_ERROR_FAILURE);
    CHECK(g_originalDataCalls == 0);

    CHECK(aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS);
    CHECK(aclrtBinaryLoadFromData(binary, sizeof(binary), &dataOptions, &dataHandle) == 81);
    CHECK(g_sentinelDataCalls == 1);
    CHECK(g_originalDataCalls == 1);
    CHECK(g_forwardedData == binary);
    CHECK(g_forwardedLength == sizeof(binary));
    CHECK(g_forwardedDataOptions == &dataOptions);
    CHECK(dataHandle == reinterpret_cast<aclrtBinHandle>(0x51));
    return 0;
}
