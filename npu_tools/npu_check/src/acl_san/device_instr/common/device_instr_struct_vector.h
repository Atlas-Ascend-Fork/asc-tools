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
constexpr uint32_t VA_REGISTER_COUNT = 8;
constexpr uint32_t VA_ENTRY_COUNT = 8;

struct VaRegisterState {
    std::array<uint16_t, VA_ENTRY_COUNT> entries{};
    uint8_t validEntries = 0;
};

struct MoveVaParamField {
    uint32_t index = 0;
    uint32_t entry = 0; // Pair starting at 0, 2, 4 or 6.
    uint64_t xn = 0;
    uint64_t xm = 0;
};

struct LdvaParamField {
    uint32_t index = 0;
    uint64_t srcAddr = 0;
    bool highHalf = false;
};

struct VtransposeParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
};

struct Vbs32ParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0;
    uint64_t dstAddr = 0;
    uint64_t scoreAddr = 0;
    uint64_t indexAddr = 0;
    uint64_t config = 0;
};

struct VmrgsortParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0;
    uint64_t dstAddr = 0;
    uint64_t srcListAddresses = 0; // Four packed 16-bit addresses in units of 8 bytes.
    uint64_t srcListLengths = 0;   // Four packed 16-bit list element counts.
    uint64_t config = 0;
};

struct VnchwconvParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0;
    uint32_t dstVaIndex = 0; // Even index of the first VA register in a pair.
    uint32_t srcVaIndex = 0;
    uint64_t config = 0;
    bool dstHighHalf = false;
    bool srcHighHalf = false;
};

struct PushPbParamField {
    std::array<uint64_t, PUSH_PB_VALUE_COUNT> values{};
};

struct VfParamField {
    uint64_t entryPc = 0;
    uint64_t instructionCount = 0;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_VECTOR_H
