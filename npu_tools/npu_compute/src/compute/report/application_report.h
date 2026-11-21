/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_APPLICATION_REPORT_H
#define NPU_COMPUTE_APPLICATION_REPORT_H
#include "artifact_sink.h"
#include "application_op_info.h"
#include <tuple>
#include "acl_pti/profiling/application_profiler.h"

namespace npucompute {
// All methods run on the ACLPTI consumer worker, including the final Commit.
class ApplicationReport final {
public:
    explicit ApplicationReport(ArtifactSink& sink) : sink_(sink) {}
    void Consume(const aclpti::profiling::ApplicationRawRecord& record);
    void Finish(const aclpti::profiling::ApplicationSummary& summary, const ApplicationOpSnapshot& snapshot = {});

private:
    void Begin();
    std::string WriteOpSummaries(
        const aclpti::profiling::ApplicationSummary& summary, const ApplicationOpSnapshot& snapshot);
    static std::string FormatManifest(
        const aclpti::profiling::ApplicationSummary& summary, const ApplicationOpSnapshot& snapshot,
        const std::string& units);
    using TaskKey = std::tuple<int32_t, uint64_t, uint32_t>;
    std::map<TaskKey, std::vector<aclpti::data::DecodedRecord>> tasks_;
    ArtifactSink& sink_;
    uint64_t records_ = 0;
    bool begun_ = false;
};
} // namespace npucompute
#endif
