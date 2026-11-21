/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_COMMON_SECTION_NAMES_H
#define NPU_COMPUTE_COMMON_SECTION_NAMES_H
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>
namespace npucompute {
// CLI enumeration, compute configuration and IPC validation share this registry.
inline constexpr std::array kSupportedSections = {
    "PipeUtilization",
    "Memory",
    "MemoryL0",
    "MemoryUB",
    "L2Cache",
    "Pipeline",
    "ArithmeticUtilization",
    "ResourceConflictRatio",
    "PmSampling",
    "SourceCounter",
    "Roofline",
    "occupancy",
    "Communication",
};
inline bool IsSupportedSection(std::string_view section)
{
    return std::find(kSupportedSections.begin(), kSupportedSections.end(), section) != kSupportedSections.end();
}
inline bool IsCommunicationMode(const std::vector<std::string>& sections)
{
    return sections.size() == 1 && sections.front() == "Communication";
}
} // namespace npucompute
#endif
