/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_SIMT_INSTRUCTION_WIDTHS_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_SIMT_INSTRUCTION_WIDTHS_H

#include "dbi/ctrlbin/ctrlbin_bindings.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aclsan {
namespace detail {

struct SimtAccessWidth {
    InstrType instruction;
    uint32_t bytes;
};

constexpr uint32_t SIMT_WORDS_PER_128_BIT_ACCESS = 2U;
constexpr uint32_t SIMT_128_BIT_ACCESS_BYTES = SIMT_WORDS_PER_128_BIT_ACCESS * sizeof(uint64_t);
constexpr SimtAccessWidth SIMT_ACCESS_WIDTHS[] = {
    {InstrType::SIMT_LDG_U8, sizeof(uint8_t)},
    {InstrType::SIMT_LDG_S8, sizeof(uint8_t)},
    {InstrType::SIMT_LDG_U16, sizeof(uint16_t)},
    {InstrType::SIMT_LDG_S16, sizeof(uint16_t)},
    {InstrType::SIMT_LDG_B32, sizeof(uint32_t)},
    {InstrType::SIMT_LDG_B64, sizeof(uint64_t)},
    {InstrType::SIMT_LDG_B128, SIMT_128_BIT_ACCESS_BYTES},
    {InstrType::SIMT_STG_B8, sizeof(uint8_t)},
    {InstrType::SIMT_STG_B16, sizeof(uint16_t)},
    {InstrType::SIMT_STG_B32, sizeof(uint32_t)},
    {InstrType::SIMT_STG_B64, sizeof(uint64_t)},
    {InstrType::SIMT_STG_B128, SIMT_128_BIT_ACCESS_BYTES},
    {InstrType::SIMT_LD_U8, sizeof(uint8_t)},
    {InstrType::SIMT_LD_S8, sizeof(uint8_t)},
    {InstrType::SIMT_LD_U16, sizeof(uint16_t)},
    {InstrType::SIMT_LD_S16, sizeof(uint16_t)},
    {InstrType::SIMT_LD_B32, sizeof(uint32_t)},
    {InstrType::SIMT_ST_B8, sizeof(uint8_t)},
    {InstrType::SIMT_ST_B16, sizeof(uint16_t)},
    {InstrType::SIMT_ST_B32, sizeof(uint32_t)},
    {InstrType::RED_G_BF16, sizeof(uint16_t)},
    {InstrType::RED_G_U32, sizeof(uint32_t)},
    {InstrType::RED_G_F16X2, sizeof(uint32_t)},
    {InstrType::RED_G_S32, sizeof(uint32_t)},
    {InstrType::RED_G_FP32, sizeof(uint32_t)},
    {InstrType::RED_G_BF16X2, sizeof(uint32_t)},
    {InstrType::RED_G_FP16, sizeof(uint16_t)},
    {InstrType::ATOM_EXCH_G_U64, sizeof(uint64_t)},
    {InstrType::ATOM_MIN_G_U64, sizeof(uint64_t)},
    {InstrType::ATOM_ADD_G_U64, sizeof(uint64_t)},
    {InstrType::ATOM_CAS_G_U64, sizeof(uint64_t)},
    {InstrType::ATOM_MAX_G_U64, sizeof(uint64_t)},
    {InstrType::ATOM_ADD_G_S64, sizeof(uint64_t)},
    {InstrType::ATOM_MIN_G_S64, sizeof(uint64_t)},
    {InstrType::ATOM_MAX_G_S64, sizeof(uint64_t)},
    {InstrType::ATOM_EXCH_G_U32, sizeof(uint32_t)},
    {InstrType::ATOM_MIN_G_U32, sizeof(uint32_t)},
    {InstrType::ATOM_CAS_G_U32, sizeof(uint32_t)},
    {InstrType::ATOM_MAX_G_U32, sizeof(uint32_t)},
    {InstrType::ATOM_ADD_G_U32, sizeof(uint32_t)},
    {InstrType::ATOM_ADD_G_S32, sizeof(uint32_t)},
    {InstrType::ATOM_MIN_G_S32, sizeof(uint32_t)},
    {InstrType::ATOM_MAX_G_S32, sizeof(uint32_t)},
};

constexpr auto SIMT_INSTRUCTION_WIDTHS = [] {
    std::array<uint32_t, static_cast<std::size_t>(InstrType::SIMT_END) + 1U> widths{};
    for (const auto& access : SIMT_ACCESS_WIDTHS) {
        widths[static_cast<std::size_t>(access.instruction)] = access.bytes;
    }
    return widths;
}();

} // namespace detail

constexpr uint32_t SimtInstructionElementBytes(uint32_t instructionId) noexcept
{
    return instructionId < detail::SIMT_INSTRUCTION_WIDTHS.size() ? detail::SIMT_INSTRUCTION_WIDTHS[instructionId] : 0U;
}

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_SIMT_INSTRUCTION_WIDTHS_H
