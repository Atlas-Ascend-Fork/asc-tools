/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_ACL_PTI_DATA_PMU_EVENTS_H
#define NPU_COMPUTE_ACL_PTI_DATA_PMU_EVENTS_H

#include <cstdint>

namespace aclpti::data::pmu {

// Hardware event IDs shared by ArithmeticUtilization collection and metric calculation.
inline constexpr uint32_t CUBE_TOTAL_INSTRUCTION_EVENT = 768;
inline constexpr uint32_t CUBE_FP_INSTRUCTION_EVENT = 789;
inline constexpr uint32_t CUBE_INT_INSTRUCTION_EVENT = 790;
inline constexpr uint32_t CUBE_FP_ACTIVE_CYCLES_EVENT = 808;
inline constexpr uint32_t CUBE_INT_ACTIVE_CYCLES_EVENT = 809;
inline constexpr uint32_t CUBE_ACTIVE_CYCLES_EVENT = 810;
inline constexpr uint32_t VECTOR_ACTIVE_CYCLES_EVENT = 1281;
inline constexpr uint32_t SIMD_VF_ISSUE_CYCLES_EVENT = 1282;
inline constexpr uint32_t VECTOR_SFU_ACTIVE_CYCLES_EVENT = 1283;
inline constexpr uint32_t SIMT_VF_ISSUE_CYCLES_EVENT = 1284;
inline constexpr uint32_t SIMD_INSTRUCTION_ISSUE_EVENT = 1320;
inline constexpr uint32_t SIMT_EXU_INSTRUCTION_ISSUE_EVENT = 1466;
inline constexpr uint32_t SIMT_DVG_INSTRUCTION_ISSUE_EVENT = 1467;
inline constexpr uint32_t SIMT_DCU_INSTRUCTION_ISSUE_EVENT = 1468;

// dav-3510 memory and pipeline event IDs used by the report metrics.
inline constexpr uint32_t SCALAR_ACTIVE_CYCLES_EVENT = 0x001U;
inline constexpr uint32_t ICACHE_MISS_EVENT = 0x034U;
inline constexpr uint32_t ICACHE_ACCESS_EVENT = 0x035U;
inline constexpr uint32_t L0A_READ_EVENT = 0x304U;
inline constexpr uint32_t L0A_WRITE_EVENT = 0x703U;
inline constexpr uint32_t L0B_READ_EVENT = 0x306U;
inline constexpr uint32_t L0B_WRITE_EVENT = 0x705U;
inline constexpr uint32_t L0C_WRITE_EVENT = 0x308U;
inline constexpr uint32_t L0C_READ_EVENT = 0x30AU;
inline constexpr uint32_t GM_TO_L1_EVENT = 0x422U;
inline constexpr uint32_t L0C_TO_GM_EVENT = 0x423U;
inline constexpr uint32_t L0A_READ_HIT_EVENT = 0x424U;
inline constexpr uint32_t L0A_READ_MISS_EVENT = 0x425U;
inline constexpr uint32_t L0A_READ_OTHER_EVENT = 0x426U;
inline constexpr uint32_t L0A_READ_HIT_ALT_EVENT = 0x427U;
inline constexpr uint32_t L0A_READ_MISS_ALT_EVENT = 0x428U;
inline constexpr uint32_t L0A_READ_OTHER_ALT_EVENT = 0x429U;
inline constexpr uint32_t L0A_WRITE_HIT_EVENT = 0x42AU;
inline constexpr uint32_t L0A_WRITE_MISS_EVENT = 0x42BU;
inline constexpr uint32_t L0A_WRITE_OTHER_EVENT = 0x42CU;
inline constexpr uint32_t L0A_WRITE_HIT_ALT_EVENT = 0x42DU;
inline constexpr uint32_t L0A_WRITE_MISS_ALT_EVENT = 0x42EU;
inline constexpr uint32_t L0A_WRITE_OTHER_ALT_EVENT = 0x42FU;
inline constexpr uint32_t UB_TO_GM_EVENT = 0x57FU;
inline constexpr uint32_t UB_TO_GM_ALT_EVENT = 0x580U;
inline constexpr uint32_t MTE2_INSTRUCTION_EVENT = 0x200U;
inline constexpr uint32_t MTE3_INSTRUCTION_EVENT = 0x201U;
inline constexpr uint32_t MTE2_ACTIVE_CYCLES_EVENT = 0x202U;
inline constexpr uint32_t MTE3_ACTIVE_CYCLES_EVENT = 0x203U;
inline constexpr uint32_t MTE1_INSTRUCTION_EVENT = 0x700U;
inline constexpr uint32_t MTE1_ACTIVE_CYCLES_EVENT = 0x702U;
inline constexpr uint32_t L1_READ_EVENT = 0x707U;
inline constexpr uint32_t L1_WRITE_EVENT = 0x709U;
inline constexpr uint32_t L0C_TO_L1_EVENT = 0x70EU;
inline constexpr uint32_t FIXPIPE_ACTIVE_CYCLES_EVENT = 0x714U;
inline constexpr uint32_t FIXPIPE_TRANSFER_EVENT = 0x717U;
inline constexpr uint32_t FIXPIPE_WRITE_EVENT = 0x70CU;
inline constexpr uint32_t UB_READ_EVENT = 0x571U;
inline constexpr uint32_t UB_WRITE_EVENT = 0x572U;

} // namespace aclpti::data::pmu

#endif // NPU_COMPUTE_ACL_PTI_DATA_PMU_EVENTS_H
