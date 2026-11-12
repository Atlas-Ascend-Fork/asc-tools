/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_INITCHECK_PADDING_H
#define NPU_TOOLS_NPU_CHECK_INITCHECK_PADDING_H

#include "acl_san/aclsan_cbdata_device.h"
#include <algorithm>

namespace npucheck {
// Visit only required bytes within a physical read interval. Malformed or absent
// metadata conservatively requires every byte. The predicate checks initialization,
// including visibility, rather than treating padding as an initializing write.
template <typename Initialized>
bool RequiredReadInitialized(
    const AclsanDeviceMemoryAccessData& data, uint64_t begin, uint64_t end, Initialized initialized)
{
    // Modeled Nz row widths: L0A/L0B inputs and the L0C accumulator, respectively.
    constexpr uint32_t kCubeInputNzRowBytes = 32;
    constexpr uint32_t kCubeAccumulatorNzRowBytes = 64;
    const auto& nz = data.validNz;
    const bool supportedSpace = data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0A ||
                                data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0B ||
                                data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0C;
    if (!supportedSpace || (nz.rowBytes != kCubeInputNzRowBytes && nz.rowBytes != kCubeAccumulatorNzRowBytes) ||
        nz.reserved != 0 || nz.rows == 0 || nz.rowDataBytes == 0 || nz.blockStride == 0 ||
        nz.blockStride % nz.rowBytes != 0 || nz.rows > nz.blockStride / nz.rowBytes || begin < nz.baseAddress) {
        return initialized(begin, end);
    }
    while (begin < end) {
        const uint64_t offset = begin - nz.baseAddress;
        const uint64_t block = offset / nz.blockStride;
        const uint64_t row = offset % nz.blockStride / nz.rowBytes;
        const uint64_t column = offset % nz.rowBytes;
        const uint64_t length = std::min(end - begin, uint64_t(nz.rowBytes) - column);
        // Compare block first to avoid multiplication overflow from invalid metadata.
        if (row < nz.rows && block <= (nz.rowDataBytes - 1) / nz.rowBytes) {
            const uint64_t remaining = nz.rowDataBytes - block * nz.rowBytes;
            if (column < remaining && !initialized(begin, begin + std::min(length, remaining - column))) {
                return false;
            }
        }
        begin += length;
    }
    return true;
}
} // namespace npucheck
#endif
