/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "report/report_writer.h"

namespace npucompute {

aclptiResult WritePmuReport(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections, const ReportConfig& config,
    const KernelMetadata& metadata, ArtifactSink& sink, std::string_view artifactPrefix, bool* degraded)
{
    try {
        const auto status = WritePmuCsv(result, sections, config, sink, artifactPrefix, degraded);
        if (status != ACLPTI_SUCCESS) {
            return status;
        }
        return WriteSummaryJsonl(result, sections, config, metadata, sink, artifactPrefix);
    } catch (const std::exception& error) {
        ASCTOOL_ERROR(
            "[npu-compute] report failed: %s"
            " code=%u",
            error.what(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACLPTI_ERROR_INTERNAL;
    }
}

} // namespace npucompute
