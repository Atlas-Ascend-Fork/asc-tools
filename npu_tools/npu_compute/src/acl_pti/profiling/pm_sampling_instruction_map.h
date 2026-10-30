/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_PM_SAMPLING_INSTRUCTION_MAP_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_PM_SAMPLING_INSTRUCTION_MAP_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace aclpti::profiling {

// Business and SIMT tables contain original ELF addresses; function entries locate the launch image.
struct PmSamplingInstructionMap {
    std::map<std::uint64_t, std::uint64_t> business; // original instruction address -> same address
    std::set<std::uint64_t> simt;                    // original business PCs in VF_SIMT functions
    std::map<std::string, std::uint64_t> functionEntries;
    std::map<std::uint64_t, std::uint64_t> relocatedEntries; // original function entry -> companion entry
    std::string error; // Mapping failure does not invalidate a successfully instrumented ELF.
};

// Pipeline and PmSampling resolve companion launch entries independently of PC decoding.
bool BuildPmSamplingFunctionMap(
    const std::vector<char>& original, const std::vector<char>& companion, PmSamplingInstructionMap& output,
    std::string& diagnostic);

bool BuildPmSamplingOriginalInstructionMap(
    const std::vector<char>& original, const std::string& disassembly, PmSamplingInstructionMap& output,
    std::string& diagnostic);

} // namespace aclpti::profiling

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_PM_SAMPLING_INSTRUCTION_MAP_H
