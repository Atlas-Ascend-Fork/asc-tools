/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_VECTOR_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_VECTOR_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace aclsan {

inline constexpr std::size_t PUSH_PB_VALUE_COUNT = 4;        // 每条 PUSH_PB 携带的状态值数量。
inline constexpr std::size_t MAX_PUSH_PB_RECORDS_PER_VF = 4; // 每个 VF 最多接收的 PUSH_PB 记录数量。
// 每个 VF 传给 HiBit 的 PUSH_PB 状态总数，等于每条记录的状态值数量乘以最大记录数量。
inline constexpr std::size_t PUSH_PB_STATE_COUNT_PER_VF = PUSH_PB_VALUE_COUNT * MAX_PUSH_PB_RECORDS_PER_VF;

struct PushPbParamField {
    std::array<uint64_t, PUSH_PB_VALUE_COUNT> values{};
};

struct VfParamField {
    uint64_t entryPc = 0;
    uint64_t instructionCount = 0;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_VECTOR_H
