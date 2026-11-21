/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_APPLICATION_SUMMARY_WRITER_H
#define NPU_COMPUTE_APPLICATION_SUMMARY_WRITER_H
#include "application_op_info.h"
#include "acl_pti/profiling/application_profiler.h"
#include "artifact_sink.h"

namespace npucompute {
struct ApplicationTaskAssociation {
    const std::vector<aclpti::data::DecodedRecord>* records = nullptr;
    const char* unavailableReason = "";
};

// Returns the manifest's result-unit object after committing the summary artifact.
std::string WriteApplicationOpSummary(
    const ApplicationOpInfo& op, const ApplicationTaskAssociation& association,
    const aclpti::profiling::ApplicationSummary& summary, ArtifactSink& sink);
} // namespace npucompute
#endif
