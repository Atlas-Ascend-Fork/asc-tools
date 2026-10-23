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

// Keys are companion ELF addresses; business values are original ELF addresses.
struct PmSamplingInstructionMap {
    std::map<std::uint64_t, std::uint64_t> business; // companion -> original
    std::set<std::uint64_t> excluded;                // probe, added ABI instructions and non-business padding/old text
    std::map<std::string, std::uint64_t> functionEntries;
    std::map<std::uint64_t, std::uint64_t> relocatedEntries; // original function entry -> companion entry
    std::string error; // Mapping failure does not invalidate a successfully instrumented ELF.
};

// Function entry mapping is independent of per-instruction classification so
// Pipeline can still use a companion when PmSampling instruction matching fails.
bool BuildPmSamplingFunctionMap(
    const std::vector<char>& original, const std::vector<char>& companion, PmSamplingInstructionMap& output,
    std::string& diagnostic);

// Accepts only a unique, order-preserving match of all surviving original instructions.
// Unsupported rewrites, missing symbols, overlapping address domains and ambiguous matches fail closed.
bool BuildPmSamplingInstructionMap(
    const std::vector<char>& original, const std::vector<char>& companion, const std::string& originalDisassembly,
    const std::string& companionDisassembly, PmSamplingInstructionMap& output, std::string& diagnostic);

} // namespace aclpti::profiling

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_PM_SAMPLING_INSTRUCTION_MAP_H
