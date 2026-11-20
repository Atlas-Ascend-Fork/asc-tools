/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_TYPES_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_TYPES_H
#include "acl_san/aclsan_cbdata_device.h"
#include "checker/memory_access_mode.h"
#include <array>
#include <memory>
#include <cstddef>
#include <cstdint>
#include "checker/racecheck/analysis_budget.h"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace npucheck::race {
// L0C unit ownership: retain ownership or hand it to the other pipeline.
inline constexpr uint32_t UNIT_FLAG_MODE_START = 2;
inline constexpr uint32_t UNIT_FLAG_MODE_END = 3;
// deviceId, executor blockType, executor blockId, executor phyCoreId.
using Domain = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
inline Domain DomainOf(const AclsanDeviceEventHeader& h)
{
    return {h.deviceId, h.executor.blockType, h.executor.blockId, h.executor.phyCoreId};
}
// Execution domain, outer instrExecId, optional vfInstrExecSeq, launchId.
// An absent sequence denotes an outer trace instruction; VF sequence zero is valid.
using Instruction = std::tuple<Domain, uint64_t, std::optional<uint64_t>, uint64_t>;
// Parent identity of one dynamic VF invocation. The internal instruction
// sequence is deliberately excluded: different VLD/VST/SLD/SST records with
// the same parent belong to one VF group for external-race filtering.
using VfKey = std::tuple<Domain, uint64_t, uint64_t>;
inline Instruction Key(const AclsanDeviceEventHeader& h)
{
    return {DomainOf(h), h.instrExecId, std::nullopt, h.launchId};
}
inline Instruction Key(const AclsanDeviceMemoryAccessData& a)
{
    if (a.vfInstrValid != 0U) {
        return {DomainOf(a.header), a.header.instrExecId, a.vfInstrExecSeq, a.header.launchId};
    }
    return Key(a.header);
}
inline std::optional<VfKey> DynamicVfKey(const AclsanDeviceMemoryAccessData& a)
{
    if (a.vfInstrValid == 0) {
        return std::nullopt;
    }
    return VfKey{DomainOf(a.header), a.header.instrExecId, a.header.launchId};
}
inline bool SameDynamicVf(const AclsanDeviceMemoryAccessData& lhs, const AclsanDeviceMemoryAccessData& rhs)
{
    const auto left = DynamicVfKey(lhs);
    const auto right = DynamicVfKey(rhs);
    return left.has_value() && right.has_value() && *left == *right;
}
inline Instruction Key(const AclsanDeviceBarrierData& b)
{
    if (b.barrierKind == ACLSAN_DEVICE_BARRIER_LOCAL_MEM) {
        return {DomainOf(b.header), b.header.instrExecId, b.vfInstrExecSeq, b.header.launchId};
    }
    return Key(b.header);
}
struct Interval {
    uint64_t begin;
    uint64_t end;
};
enum class NormalizationStatus { Complete, InvalidLayout, SegmentBudget };
// Validated geometry used to share immutable ranges across accesses.
struct NormalizationObservation {
    bool empty = false;
    uint64_t base = 0;
    uint64_t bytes = 0;
    std::vector<std::pair<uint64_t, uint64_t>> sparseAxes;
};
NormalizationStatus DescribeNormalization(
    const AclsanDeviceMemoryAccessData& access, NormalizationObservation& observation);
NormalizationStatus NormalizeDetailed(
    const AclsanDeviceMemoryAccessData& access, size_t budget, std::vector<Interval>& result);
bool Normalize(const AclsanDeviceMemoryAccessData& access, size_t budget, std::vector<Interval>& result);
void MergeIntervals(std::vector<Interval>& ranges);
bool ValidPipe(uint32_t pipe);
// Atomic destinations read the old bytes and then write the result. The API
// enumerates modes; they are not a READ/WRITE bitmask.
inline bool ReadsMemory(uint32_t mode)
{
    mode = NormalizeMemoryAccessMode(mode);
    return mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ || (mode >= ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD &&
                                                        mode <= ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX);
}
inline bool WritesMemory(uint32_t mode)
{
    mode = NormalizeMemoryAccessMode(mode);
    return mode >= ACLSAN_DEVICE_MEMORY_ACCESS_WRITE && mode <= ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX;
}
// Adapt ACLSan cross-core callbacks to the SET/WAIT representation used by dependency graphs.
AclsanDeviceSyncData NormalizeSync(const AclsanDeviceSyncData& data);

using Storage = std::tuple<uint32_t, Domain>;
Storage StorageOf(const AclsanDeviceMemoryAccessData& access);
using UnitResetHistory = std::map<Domain, std::set<uint64_t>>;
UnitResetHistory UnitFlagResets(
    const std::vector<AclsanDeviceRegisterStateData>& registers, std::set<std::string>& reasons);
uint64_t UnitFlagEpoch(const AclsanDeviceEventHeader& header, const UnitResetHistory& resets);
} // namespace npucheck::race
#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_TYPES_H
