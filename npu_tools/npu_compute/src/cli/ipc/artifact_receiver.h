/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_ARTIFACT_RECEIVER_H
#define NPU_COMPUTE_ARTIFACT_RECEIVER_H
#include "ipc/ipc.h"
#include <boost/filesystem/path.hpp>
#include <string>
namespace npucompute::cli {
class ArtifactReceiver {
public:
    explicit ArtifactReceiver(const std::string& directory);
    ~ArtifactReceiver();
    ArtifactReceiver(const ArtifactReceiver&) = delete;
    ArtifactReceiver& operator=(const ArtifactReceiver&) = delete;
    void Receive(const ipc::Frame& frame);
    bool Validate(
        const ipc::Result& result, const ipc::Config& config, std::vector<std::string>* issues = nullptr) const;
    size_t Count() const { return manifest_.size(); }

private:
    void Abort() noexcept;
    void OpenArtifactDirectory();
    void BeginArtifact(const ipc::Frame& frame);
    void ReceiveChunk(const ipc::Frame& frame);
    void EndArtifact(const ipc::Frame& frame);
    boost::filesystem::path directory_path_;
    boost::filesystem::path active_directory_path_;
    int directory_ = -1;
    int active_directory_ = -1;
    int file_ = -1;
    ipc::ArtifactBegin active_;
    std::string temporary_;
    std::string active_name_;
    uint64_t offset_ = 0;
    std::vector<ipc::ManifestEntry> manifest_;
};
} // namespace npucompute::cli
#endif
