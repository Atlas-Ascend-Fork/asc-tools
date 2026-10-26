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
#include <vector>

#include <optional>
#include <string_view>

namespace npucompute {

enum class MissingReason {
    None,
    Unknown,
    CoreTypeNotApplicable,
    EventMissing,
    InvalidDenominator,
    InvalidFrequencyOrDuration,
    DbiUnavailable,
    UnsupportedSocBandwidth,
    RowSizeMismatch,
};

struct PmuMetricField {
    std::string text;
    MissingReason missingReason = MissingReason::None;
    std::optional<double> value;
    bool integer = false;
};

using PmuMetricRow = std::vector<PmuMetricField>;

class PmuMetricBuilder final {
public:
    static std::vector<std::string> Header(std::string_view section, PmuDataLevel level = PmuDataLevel::Block);
    static PmuMetricRow BuildBlock(
        std::string_view section, const aclptiBlockKey& key, const aclptiPmuDataRow& row, const ReportConfig& config,
        std::optional<double> duration = std::nullopt);
    static PmuMetricRow BuildTask(
        std::string_view section, const aclptiTaskPmuData& taskPmuData, const ReportConfig& config,
        std::optional<double> duration = std::nullopt);
    static std::optional<double> TaskDuration(const aclptiProfilingDataResult& result);
    static std::optional<double> OperationDuration(const aclptiProfilingDataResult& result, PmuDataLevel level);
    static const char* ReasonName(MissingReason reason);
};

} // namespace npucompute
