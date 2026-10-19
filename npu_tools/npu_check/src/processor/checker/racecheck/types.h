/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_CHECK_RACECHECK_TYPES_H
#define NPU_CHECK_RACECHECK_TYPES_H
#include "acl_san/aclsan_cbdata_device.h"
#include <array>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace npucheck::race {
using Domain = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
inline Domain DomainOf(const AclsanDeviceEventHeader& h) { return {h.deviceId, h.blockType, h.blockId, h.phyCoreId}; }
using Instruction = std::tuple<Domain, uint64_t>;
inline Instruction Key(const AclsanDeviceEventHeader& h) { return {DomainOf(h), h.instrExecId}; }
struct Interval {
    uint64_t begin;
    uint64_t end;
};
struct Limits {
    size_t events = 100000;
    size_t segments = 1U << 20U;
    size_t edges = 1U << 20U;
    size_t comparisons = 10000000;
    size_t queryWork = 10000000;
    size_t queryCache = 65536;
    size_t stateSegments = 1U << 20U;
    size_t stateBytes = 128U << 20U;
};
bool Normalize(const AclsanDeviceMemoryAccessData& access, size_t budget, std::vector<Interval>& result);
void MergeIntervals(std::vector<Interval>& intervals);
bool ValidPipe(uint32_t pipe);

using Storage = std::tuple<uint32_t, Domain>;
Storage StorageOf(const AclsanDeviceMemoryAccessData& access);
} // namespace npucheck::race
#endif
