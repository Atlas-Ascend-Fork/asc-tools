/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file ascendc_acl_stub.cpp
 * \brief
 */
#ifdef ASCENDC_CPU_DEBUG
#include <link.h>
#include <string.h>
#include "securec.h"
#include "acl/acl.h"
#include "acl/acl_prof.h"
#include "stub_def.h"
#include "kernel_fp16.h"
#include "kernel_elf_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

aclError aclprofInit(const char* profilerResultPath, size_t length)
{
    (void)profilerResultPath;
    (void)length;
    return ACL_SUCCESS;
}

aclError aclprofSetConfig(aclprofConfigType configType, const char* config, size_t configLength)
{
    (void)configType;
    (void)config;
    (void)configLength;
    return ACL_SUCCESS;
}

aclError aclprofStart(const aclprofConfig* profilerConfig)
{
    (void)profilerConfig;
    return ACL_SUCCESS;
}

aclError aclprofStop(const aclprofConfig* profilerConfig)
{
    (void)profilerConfig;
    return ACL_SUCCESS;
}

aclError aclprofFinalize() { return ACL_SUCCESS; }

aclError aclInit(const char* configPath)
{
    (void)configPath;
    return ACL_SUCCESS;
}

aclError aclFinalize() { return ACL_SUCCESS; }

aclError aclrtGetVersion(int32_t* majorVersion, int32_t* minorVersion, int32_t* patchVersion)
{
    (void)majorVersion;
    (void)minorVersion;
    (void)patchVersion;
    return ACL_SUCCESS;
}

size_t aclDataTypeSize(aclDataType dataType)
{
    switch (dataType) {
        case ACL_FLOAT:
            return 4;
        case ACL_FLOAT16:
            return 2;
        case ACL_INT8:
            return 1;
        case ACL_INT16:
            return 2;
        case ACL_INT32:
            return 4;
        case ACL_INT64:
            return 8;
        case ACL_UINT8:
            return 1;
        case ACL_UINT16:
            return 2;
        case ACL_UINT32:
            return 4;
        case ACL_UINT64:
            return 8;
        case ACL_DOUBLE:
            return 8;
        case ACL_BOOL:
            return 1;
        default:
            return 0;
    }
}

float aclFloat16ToFloat(aclFloat16 value)
{
    half fp16;
    fp16.val = value;
    return fp16.ToFloat();
}

aclFloat16 aclFloatToFloat16(float value) { return half(value).val; }

aclError aclrtSetDevice(int32_t deviceId)
{
    (void)deviceId;
    return ACL_SUCCESS;
}

aclError aclrtResetDevice(int32_t deviceId)
{
    (void)deviceId;
    return ACL_SUCCESS;
}

aclError aclrtCreateStream(aclrtStream* stream)
{
    (void)stream;
    return ACL_SUCCESS;
}

aclError aclrtCreateStreamWithConfig(aclrtStream* stream, uint32_t priority, uint32_t flag)
{
    (void)priority;
    (void)flag;
    (void)stream;
    return ACL_SUCCESS;
}

aclError aclrtDestroyStream(aclrtStream stream)
{
    (void)stream;
    return ACL_SUCCESS;
}

aclError aclrtDestroyStreamForce(aclrtStream stream)
{
    (void)stream;
    return ACL_SUCCESS;
}

aclError aclrtSynchronizeStream(aclrtStream stream)
{
    (void)stream;
    return ACL_SUCCESS;
}

aclError aclrtMalloc(void** devPtr, size_t size, aclrtMemMallocPolicy policy)
{
    (void)policy;
    if (size == 0 || devPtr == nullptr) {
        printf("[ERROR] The parameter is invalid.\n");
        return ACL_ERROR_INVALID_PARAM;
    }
    *devPtr = AscendC::GmAlloc(size);
    return ACL_SUCCESS;
}

aclError aclrtFree(void* devPtr)
{
    if (devPtr == nullptr) {
        printf("[ERROR] The parameter is invalid.\n");
        return ACL_ERROR_INVALID_PARAM;
    }
    AscendC::GmFree(devPtr);
    return ACL_SUCCESS;
}

aclError aclrtMallocHost(void** hostPtr, size_t size)
{
    if (size == 0 || hostPtr == nullptr) {
        printf("[ERROR] The parameter is invalid.\n");
        return ACL_ERROR_INVALID_PARAM;
    }
    *hostPtr = AscendC::GmAlloc(size);
    return ACL_SUCCESS;
}

aclError aclrtFreeHost(void* hostPtr)
{
    if (hostPtr == nullptr) {
        printf("[ERROR] The parameter is invalid.\n");
        return ACL_ERROR_INVALID_PARAM;
    }
    AscendC::GmFree(hostPtr);
    return ACL_SUCCESS;
}

aclError aclrtMemset(void* devPtr, size_t maxCount, int32_t value, size_t count)
{
    if (devPtr == nullptr) {
        printf("[ERROR] The parameter is invalid.\n");
        return ACL_ERROR_INVALID_PARAM;
    }
    if (count > maxCount) {
        printf("[ERROR] The maxCount is %ld", maxCount);
    }
    auto ret = memset_s(devPtr, maxCount, value, count);
    if (ret != EOK) {
        return ACL_ERROR_BAD_ALLOC;
    }
    return ACL_SUCCESS;
}

aclError aclrtMemsetAsync(void* devPtr, size_t maxCount, int32_t value, size_t count, aclrtStream stream)
{
    (void)stream;
    return aclrtMemset(devPtr, maxCount, value, count);
}

aclError aclrtMemcpy(void* dst, size_t destMax, const void* src, size_t count, aclrtMemcpyKind kind)
{
    (void)kind;
    if (dst == nullptr || src == nullptr) {
        printf("[ERROR] The parameter is invalid.\n");
        return ACL_ERROR_INVALID_PARAM;
    }
    auto ret = memcpy_s(dst, destMax, src, count);
    if (ret != EOK) {
        return ACL_ERROR_BAD_ALLOC;
    }
    return ACL_SUCCESS;
}

aclError aclrtMemcpyAsync(
    void* dst, size_t destMax, const void* src, size_t count, aclrtMemcpyKind kind, aclrtStream stream)
{
    (void)stream;
    return aclrtMemcpy(dst, destMax, src, count, kind);
}

aclError aclrtMemcpy2d(
    void* dst, size_t dpitch, const void* src, size_t spitch, size_t width, size_t height, aclrtMemcpyKind kind)
{
    (void)kind;
    if (dst == nullptr || src == nullptr || width > spitch) {
        printf("[ERROR] The parameter is invalid.\n");
        return ACL_ERROR_INVALID_PARAM;
    }
    auto* dstBytes = static_cast<char*>(dst);
    const auto* srcBytes = static_cast<const char*>(src);
    for (size_t i = 0; i < height; ++i) {
        auto ret = memcpy_s(dstBytes + i * dpitch, dpitch, srcBytes + i * spitch, width);
        if (ret != EOK) {
            return ACL_ERROR_BAD_ALLOC;
        }
    }
    return ACL_SUCCESS;
}

aclError aclrtMemcpy2dAsync(
    void* dst, size_t dpitch, const void* src, size_t spitch, size_t width, size_t height, aclrtMemcpyKind kind,
    aclrtStream stream)
{
    (void)stream;
    return aclrtMemcpy2d(dst, dpitch, src, spitch, width, height, kind);
}

aclError aclrtCreateContext(aclrtContext* context, int32_t deviceId)
{
    (void)context;
    (void)deviceId;
    return ACL_SUCCESS;
}

aclError aclrtDestroyContext(aclrtContext context)
{
    (void)context;
    return ACL_SUCCESS;
}

aclError aclrtBinaryLoadFromData(
    const void* data, size_t length, const aclrtBinaryLoadOptions* options, aclrtBinHandle* binHandle)
{
    (void)options;
    (void)binHandle;
    if (data == nullptr || length == 0) {
        printf(
            "[ERROR] During binary load, get invalid elfData or data length, may be caused by a compilation error.\n");
        return ACL_ERROR_INVALID_PARAM;
    }

    try {
        AscendC::RegisterKernelElf(reinterpret_cast<const uint8_t*>(data), length);
    } catch (const std::exception& e) {
        printf("[ERROR] During binary load, failed to register kernel elf data: %s\n", e.what());
        return ACL_ERROR_INVALID_PARAM;
    }
    return ACL_SUCCESS;
}

aclError aclrtSynchronizeDevice() { return ACL_SUCCESS; }
#ifdef __cplusplus
}
#endif // __cplusplus

namespace {
static int callback(struct dl_phdr_info* info, size_t size, void* data)
{
    (void)size;
    (void)data;
    std::string soName = std::string(info->dlpi_name);
    if (soName.find("libascendcl.so") != std::string::npos) {
        printf(
            "[ERROR] CPU debug supports only these ACL interfaces: "
            "[aclprofInit, aclprofSetConfig, aclprofStart, aclprofStop, aclprofFinalize, aclInit, aclFinalize, "
            "aclrtGetVersion, aclDataTypeSize, aclFloat16ToFloat, aclFloatToFloat16, aclrtSetDevice, aclrtResetDevice, "
            "aclrtCreateStream, aclrtCreateStreamWithConfig, aclrtDestroyStream, aclrtDestroyStreamForce, "
            "aclrtSynchronizeStream, aclrtMalloc, aclrtFree, aclrtMallocHost, aclrtFreeHost, aclrtMemset, "
            "aclrtMemsetAsync, aclrtMemcpy, aclrtMemcpyAsync, aclrtMemcpy2d, aclrtMemcpy2dAsync, aclrtCreateContext, "
            "aclrtDestroyContext, aclrtBinaryLoadFromData, aclrtSynchronizeDevice]\n");
        abort();
    }
    return 0;
}

__attribute__((constructor)) void check_interface()
{
    printf("[%s][%s:%d]\n", __FILE__, __FUNCTION__, __LINE__);
    dl_iterate_phdr(callback, nullptr);
}
} // namespace

#endif // ASCENDC_CPU_DEBUG
