/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "dbi/dbi_pipeline.h"

#include <set>

namespace aclsan {

std::vector<ProbeGroup> NormalizeProbeGroups(const std::vector<ProbeGroup>& groups)
{
    std::set<ProbeGroup> unique(groups.begin(), groups.end());
    // These memory operations consume configuration written by scalar SET_* instructions.
    if (unique.find(ProbeGroup::Mte2) != unique.end() || unique.find(ProbeGroup::Mte3) != unique.end() ||
        unique.find(ProbeGroup::Fixpipe) != unique.end()) {
        unique.insert(ProbeGroup::Scalar);
    }
    return {unique.begin(), unique.end()};
}

std::vector<ProbeGroup> ProbeGroupsFromMask(uint32_t mask)
{
    const std::pair<uint32_t, ProbeGroup> groups[] = {
        {PROBE_GROUP_MTE1, ProbeGroup::Mte1},         {PROBE_GROUP_MTE2, ProbeGroup::Mte2},
        {PROBE_GROUP_MTE3, ProbeGroup::Mte3},         {PROBE_GROUP_FIXPIPE, ProbeGroup::Fixpipe},
        {PROBE_GROUP_SCALAR, ProbeGroup::Scalar},     {PROBE_GROUP_SYNC, ProbeGroup::Sync},
        {PROBE_GROUP_MATRIX, ProbeGroup::Matrix},     {PROBE_GROUP_VECTOR, ProbeGroup::Vector},
        {PROBE_GROUP_BOUNDARY, ProbeGroup::Boundary},
    };
    std::vector<ProbeGroup> selected;
    for (const auto& group : groups) {
        if ((mask & group.first) != 0) {
            selected.push_back(group.second);
        }
    }
    return NormalizeProbeGroups(selected);
}

std::string ProbeGroupName(ProbeGroup group)
{
    switch (group) {
        case ProbeGroup::Mte1:
            return "mte1";
        case ProbeGroup::Mte2:
            return "mte2";
        case ProbeGroup::Mte3:
            return "mte3";
        case ProbeGroup::Fixpipe:
            return "fixpipe";
        case ProbeGroup::Scalar:
            return "scalar";
        case ProbeGroup::Sync:
            return "sync";
        case ProbeGroup::Matrix:
            return "matrix";
        case ProbeGroup::Vector:
            return "vector";
        case ProbeGroup::Boundary:
            return "boundary";
    }
    return {};
}

} // namespace aclsan
