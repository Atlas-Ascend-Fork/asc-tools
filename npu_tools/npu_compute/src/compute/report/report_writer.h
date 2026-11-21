/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#pragma once

#include "aclpti/aclpti_data.h"
#include "compute_types.h"

#include <locale>
#include <string>
#include <string_view>
#include <vector>

namespace npucompute {

class ArtifactSink;

aclptiResult WritePmuCsv(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections, const ReportConfig& config,
    ArtifactSink& sink, std::string_view artifactPrefix = {}, bool* degraded = nullptr);

aclptiResult WriteSummaryJsonl(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections, const ReportConfig& config,
    const KernelMetadata& metadata, ArtifactSink& sink, std::string_view artifactPrefix = {}, bool* degraded = nullptr);

struct FormattedOpInfoSummary {
    // The object remains open so application fields can precede its final delimiter.
    std::string fields;
    std::vector<std::string> unavailableFields;
};

// Shares field ordering, formulas and NA semantics with kernel summaries.
FormattedOpInfoSummary FormatOpInfoSummary(
    const aclptiProfilingDataResult& result, const ReportConfig& config, const KernelMetadata& metadata);

std::string QuoteJsonString(std::string_view value, const std::locale& locale);

aclptiResult WritePmuReport(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections, const ReportConfig& config,
    const KernelMetadata& metadata, ArtifactSink& sink, std::string_view artifactPrefix = {}, bool* degraded = nullptr);

} // namespace npucompute
