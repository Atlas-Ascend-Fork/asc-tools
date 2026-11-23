/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_COMMUNICATION_ARTIFACT_SCHEMA_H
#define NPU_COMPUTE_COMMUNICATION_ARTIFACT_SCHEMA_H
#include <algorithm>
#include <cstddef>
#include <string_view>
namespace npucompute::communication {
inline constexpr const char* kStagingDirectory = ".communication-staging";
inline constexpr const char* kProcessDirectory = ".communication-staging/process-uds";
inline constexpr const char* kArtifactPrefix = ".communication-staging/process-uds/";
inline constexpr const char* kTaskTracksFileName = "task-tracks.jsonl";
inline constexpr unsigned kManifestSchemaVersion = 1;
inline constexpr size_t kMaxFragmentBytes = 1024U * 1024U;
inline constexpr size_t kMaxFragments = 500;
inline constexpr size_t kMaxArtifacts = 512; // Includes manifest and hardware information.
inline bool IsRecordFileName(std::string_view name)
{
    constexpr std::string_view prefix = "records-", suffix = ".jsonl";
    if (name.size() <= prefix.size() + suffix.size() || name.substr(0, prefix.size()) != prefix ||
        name.substr(name.size() - suffix.size()) != suffix) {
        return false;
    }
    const auto number = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    return std::all_of(number.begin(), number.end(), [](char value) { return value >= '0' && value <= '9'; });
}
} // namespace npucompute::communication
#endif
