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

} // namespace aclpti::data::pmu

#endif // NPU_COMPUTE_ACL_PTI_DATA_PMU_EVENTS_H
