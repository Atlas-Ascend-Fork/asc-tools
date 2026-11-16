/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_REGISTER_DEPENDENCY_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_REGISTER_DEPENDENCY_H

#include "acl_san/aclsan_api.h"
#include "aclsan_memory_cbdata.h"
#include "aclsan_trace_buffer.h"
#include "device_instr/decoder.h"

#include <cstdint>
#include <optional>

namespace aclsan {

struct RegisterDependencyMask {
    uint64_t mask0 = 0;
    uint64_t mask1 = 0;
};

constexpr uint64_t RegisterBit(AclsanDeviceRegisterId id) noexcept
{
    const auto index = static_cast<uint32_t>(id);
    return index < 64U ? UINT64_C(1) << index : 0;
}

constexpr bool HasRegisterBit(uint64_t mask, AclsanDeviceRegisterId id) noexcept
{
    const auto bit = RegisterBit(id);
    return bit != 0 && (mask & bit) != 0;
}

RegisterDependencyMask ResolveRegisterDependencies(
    const MemoryInstructionField& field, const MemoryRegisterState& state) noexcept;

std::optional<AclsanDeviceRegisterStateData> MakeRegisterStateData(
    const ParsedTraceRecord& parsed, const DecodedInstruction& decoded) noexcept;

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_REGISTER_DEPENDENCY_H
