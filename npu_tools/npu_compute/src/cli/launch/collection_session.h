/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_COLLECTION_SESSION_H
#define NPU_COMPUTE_COLLECTION_SESSION_H
#include "config/config.h"
#include "launch/process_launcher.h"
#include "ipc/ipc.h"
namespace npucompute::cli {
ipc::Config LoadCollectionConfig(const CliConfig& config);
struct CollectionOutcome {
    std::string outcome = "infra_failed";
    std::string complete = "unknown";
    size_t artifacts = 0;
    std::string childExit = "not_started";
    std::vector<std::string> issues;
};
class CollectionSession {
public:
    int Run(ProcessLaunchRequest request, const ipc::Config& config, const std::string& directory, std::string* error);
    const CollectionOutcome& Outcome() const { return outcome_; }

private:
    CollectionOutcome outcome_;
};
} // namespace npucompute::cli
#endif
