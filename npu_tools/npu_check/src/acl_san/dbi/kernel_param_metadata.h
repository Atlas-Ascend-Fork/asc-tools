/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_KERNEL_PARAM_METADATA_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_KERNEL_PARAM_METADATA_H

#include "acl_san/aclsan_cbdata_launch.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aclsan {
struct KernelUbMetadata {
    uint32_t kernelVfType = ACLSAN_KERNEL_VF_TYPE_UNKNOWN;
    uint64_t compilerAllocUbBytes = 0;
    bool compilerAllocUbValid = false;
};

// functionName is the complete metadata entry name (without .ascend.meta.),
// not a base name to which tilingKey is appended. Callers select by name or by
// tilingKey; supplying both requires the selected entry to satisfy both filters.
bool GetKernelUbMetadata(
    const std::string& elf, const std::string& functionName, KernelUbMetadata& metadata, std::string& diagnostic,
    const uint64_t* tilingKey = nullptr);

struct BinarySectionRange {
    uint64_t offset = 0;
    uint64_t bytes = 0;
};

bool GetReadOnlyBinarySections(
    const std::string& elf, std::vector<BinarySectionRange>& ranges, std::string& diagnostic);
// 更新插桩后.o的.ascend.meta的section段，使得该内容与dbi的统一偏移保持一致
// original: 插桩前原始device elf的完整二进制内容     patched: bisheng-tune插桩后devce elf的完整二进制内容
bool ModifyKernelParamMetadata(
    const std::string& original, std::string& patched, uint32_t traceOffset, std::string& diagnostic,
    const uint64_t* tilingKey = nullptr);
bool GetMaximumKernelArgumentArea(
    const std::string& kernelElf, uint32_t& maximumArea, bool& found, std::string& diagnostic);
} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_KERNEL_PARAM_METADATA_H
