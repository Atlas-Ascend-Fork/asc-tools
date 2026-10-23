/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "runtime/section_config.h"

#include <algorithm>
#include <array>
#include <utility>

namespace npucompute {
namespace {

constexpr std::array kSupportedSections = {
    "PipeUtilization",       "Memory",     "MemoryL0", "MemoryUB", "L2Cache", "Pipeline", "ArithmeticUtilization",
    "ResourceConflictRatio", "PmSampling",
};

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool IsSupportedSection(const std::string& section)
{
    return std::find(kSupportedSections.begin(), kSupportedSections.end(), section) != kSupportedSections.end();
}

} // namespace

bool SectionConfig::Load(const std::vector<std::string>& parsed_sections, std::string* error)
{
    Reset();
    for (const auto& section : parsed_sections) {
        if (!IsSupportedSection(section)) {
            return Fail("unknown section: " + section, error);
        }
    }
    const std::size_t section_count = parsed_sections.size();
    if (section_count == 0 || section_count > kSupportedSections.size()) {
        return Fail("section count is out of range", error);
    }
    sections_ = std::move(parsed_sections);
    const bool pipelineSection = std::find(sections_.begin(), sections_.end(), "Pipeline") != sections_.end();
    const bool pmSamplingSection = std::find(sections_.begin(), sections_.end(), "PmSampling") != sections_.end();
    sections_.erase(
        std::remove_if(
            sections_.begin(), sections_.end(),
            [](const std::string& section) { return section == "Pipeline" || section == "PmSampling"; }),
        sections_.end());
    section_pointers_.clear();
    section_pointers_.reserve(sections_.size());
    for (const std::string& section : sections_) {
        section_pointers_.push_back(section.c_str());
    }
    params_.sections = section_pointers_.data();
    params_.numSections = section_pointers_.size();
    params_.blockResult = ACLPTI_BLOCK_RESULT_ALL;
    params_.collectPipeline = pipelineSection;
    params_.collectPcSampling = pmSamplingSection;
    return true;
}

aclptiRangeProfilerSetConfigParams* SectionConfig::Params() { return &params_; }

std::string SectionConfig::JoinedSections() const
{
    std::string joined;
    for (const std::string& section : sections_) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += section;
    }
    if (params_.collectPipeline) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += "Pipeline";
    }
    if (params_.collectPcSampling) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += "PmSampling";
    }
    return joined;
}

const std::vector<std::string>& SectionConfig::Sections() const { return sections_; }

bool SectionConfig::PipelineEnabled() const { return params_.collectPipeline; }

void SectionConfig::Reset()
{
    params_ = {};
    section_pointers_.clear();
    sections_.clear();
}

} // namespace npucompute
