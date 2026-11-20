/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <acl/acl.h>
#include <acl/acl_platform.h>
#include <driver/ascend_hal_base.h>
#include <driver/dsmi_common_interface.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

constexpr std::size_t kGigabyte = 1024ULL * 1024ULL * 1024ULL;

aclError CopyPlatformValue(std::string_view value, char* output, uint32_t maximumLength)
{
    if (output == nullptr || maximumLength <= value.size()) {
        return ACL_ERROR_INVALID_PARAM;
    }
    std::memcpy(output, value.data(), value.size());
    output[value.size()] = '\0';
    return ACL_SUCCESS;
}

} // namespace

extern "C" aclError aclrtGetDeviceCount(uint32_t* count)
{
    if (count == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *count = 1;
    std::fprintf(stderr, "[hardware_api_stub] aclrtGetDeviceCount\n");
    return ACL_SUCCESS;
}

extern "C" const char* aclrtGetSocName() { return "Ascend950PR_9599"; }

extern "C" aclError aclrtGetDeviceInfo(uint32_t deviceId, aclrtDevAttr attribute, std::int64_t* value)
{
    if (deviceId != 0 || value == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    switch (attribute) {
        case ACL_DEV_ATTR_AICPU_CORE_NUM:
            *value = 8;
            break;
        case ACL_DEV_ATTR_AICORE_CORE_NUM:
            *value = 32;
            break;
        case ACL_DEV_ATTR_CUBE_CORE_NUM:
            *value = 16;
            break;
        case ACL_DEV_ATTR_VECTOR_CORE_NUM:
            *value = 16;
            break;
        case ACL_DEV_ATTR_NPU_ARCH:
            *value = 3510;
            break;
        default:
            return ACL_ERROR_INVALID_PARAM;
    }
    return ACL_SUCCESS;
}

extern "C" aclError aclplatformGetDeviceInfo(aclplatformDevInfo information, char* value, uint32_t maximumLength)
{
    switch (information) {
        case ACL_PLATFORM_MEMORY_SIZE:
            return CopyPlatformValue("68719476736", value, maximumLength);
        case ACL_PLATFORM_CUBE_FREQ:
            return CopyPlatformValue("1800", value, maximumLength);
        case ACL_PLATFORM_VEC_FREQ:
            return CopyPlatformValue("1600", value, maximumLength);
        default:
            return ACL_ERROR_INVALID_PARAM;
    }
}

extern "C" drvError_t halGetDeviceInfo(
    uint32_t deviceId, std::int32_t moduleType, std::int32_t infoType, std::int64_t* value)
{
    if (deviceId != 0 || value == nullptr) {
        return DRV_ERROR_INVALID_VALUE;
    }
    if (moduleType == MODULE_TYPE_CCPU && infoType == INFO_TYPE_CORE_NUM) {
        *value = 4;
        return DRV_ERROR_NONE;
    }
    if (moduleType == MODULE_TYPE_SYSTEM && infoType == INFO_TYPE_DEV_OSC_FREQUE) {
        *value = 1000000; // kHz, matching the occupancy fixture's system counters.
        return DRV_ERROR_NONE;
    }
    if ((moduleType == MODULE_TYPE_AICORE || moduleType == MODULE_TYPE_VECTOR_CORE) &&
        infoType == INFO_TYPE_CURRENT_FREQ) {
        return DRV_ERROR_INVALID_VALUE;
    }
    if ((moduleType == MODULE_TYPE_AICORE || moduleType == MODULE_TYPE_VECTOR_CORE) && infoType == INFO_TYPE_FREQUE) {
        *value = 1650;
        return DRV_ERROR_NONE;
    }
    return DRV_ERROR_INVALID_VALUE;
}

extern "C" int dsmi_get_aicpu_info(int deviceId, struct dsmi_aicpu_info_stru* information)
{
    if (deviceId != 0 || information == nullptr) {
        return -1;
    }
    *information = {};
    information->curFreq = 1500;
    return 0;
}

extern "C" int dsmi_get_chip_info(int deviceId, struct dsmi_chip_info_stru* information)
{
    if (deviceId != 0 || information == nullptr) {
        return -1;
    }
    *information = {};
    constexpr unsigned char version[] = "V100";
    std::memcpy(information->chip_ver, version, sizeof(version));
    return 0;
}

extern "C" aclError aclrtGetMemInfo(aclrtMemAttr attribute, std::size_t* freeBytes, std::size_t* totalBytes)
{
    if (attribute != ACL_HBM_MEM || freeBytes == nullptr || totalBytes == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *freeBytes = 48ULL * kGigabyte;
    *totalBytes = 64ULL * kGigabyte;
    return ACL_SUCCESS;
}

extern "C" int dsmi_get_device_frequency(int deviceId, int deviceType, unsigned int* frequency)
{
    if (deviceId != 0 || deviceType != DSMI_DEVICE_TYPE_HBM || frequency == nullptr) {
        return -1;
    }
    *frequency = 3200;
    return 0;
}
