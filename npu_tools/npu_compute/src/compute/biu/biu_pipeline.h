/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_BIU_BIU_PIPELINE_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_BIU_BIU_PIPELINE_H

#include "aclpti/aclpti_data.h"
#include "common/trace_writer.h"

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace npucompute {

struct BiuChannelKey {
    uint64_t replayId = 0;
    int32_t deviceId = -1;
    uint8_t groupId = 0;
    aclptiBiuCoreKind coreKind = aclptiBiuCoreKind::Aic;
};

struct BiuClockConfig {
    double syscntFrequencyHz = 0.0;
    double aicFrequencyHz = 0.0;
    double aivFrequencyHz = 0.0;
    std::string source;
};

enum class BiuPipe : uint8_t { Scalar, Vector, Cube, Mte1, Mte2, Mte3, Fixp };

struct BiuTraceEvent {
    BiuChannelKey channel;
    BiuPipe pipe = BiuPipe::Scalar;
    uint16_t blockId = 0;
    std::string name;
    std::string color;
    double startUs = 0.0;
    double durationUs = 0.0;
    bool isDfx = false;
};

using BiuTraceEventSink = std::function<aclptiResult(const BiuTraceEvent&)>;

struct BiuParseStats {
    uint64_t payloadBytes = 0;
    uint64_t paddingBytes = 0;
    uint64_t intervalCount = 0;
    uint64_t dfxEventCount = 0;
    uint64_t invalidRecordCount = 0;
    uint64_t incompleteIntervalCount = 0;
};

aclptiResult ParsePipelineReplay(
    uint64_t replayId, const aclptiPipelineData& replay, const BiuClockConfig& clocks, const BiuTraceEventSink& sink,
    BiuParseStats* stats);

class PipeTraceWriter {
public:
    PipeTraceWriter() = default;
    ~PipeTraceWriter();

    PipeTraceWriter(const PipeTraceWriter&) = delete;
    PipeTraceWriter& operator=(const PipeTraceWriter&) = delete;

    aclptiResult Begin(const boost::filesystem::path& outputPath, std::string* error = nullptr);
    aclptiResult Append(const BiuTraceEvent& event);
    aclptiResult Commit();
    void Abort() noexcept;
    uint64_t EventCount() const;

private:
    enum class State { Created, Writing, Committed, Failed };

    State state_ = State::Created;
    std::unique_ptr<TraceFileOutput> output_;
    std::unique_ptr<TraceWriter> trace_;
    uint64_t eventCount_ = 0;
};

struct PipeTraceFragmentInfo {
    uint64_t resultSequence = 0;
    uint64_t replayId = 0;
    int32_t deviceId = -1;
    std::string file;
    uint64_t eventCount = 0;
};

aclptiResult CreatePipeTraceProcessStaging(
    const boost::filesystem::path& collectionDirectory, boost::filesystem::path* processDirectory,
    std::string* error = nullptr);

aclptiResult WritePipeTraceManifest(
    const boost::filesystem::path& processDirectory, const std::string& state, aclptiResult status,
    const std::vector<PipeTraceFragmentInfo>& fragments, const std::string& failureMessage = {});

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_BIU_BIU_PIPELINE_H
