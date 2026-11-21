/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_COMMUNICATION_COLLECTOR_H
#define NPU_COMPUTE_COMMUNICATION_COLLECTOR_H
#include "aclpti/aclpti_communication.h"
#include "aclpti/aclpti_types.h"
#include "artifact_sink.h"
#include <mutex>
#include <utility>
#include <vector>
namespace npucompute {
struct CommunicationBatch {
    std::vector<aclpti::communication::Record> records;
    std::vector<aclpti::communication::DeviceClocks> clocks;
    std::string diagnostic;
    aclptiResult status = ACLPTI_SUCCESS;
    uint64_t errors = 0;
    uint64_t dropped = 0;
};

class CommunicationCollector final {
public:
    CommunicationCollector(ArtifactSink& sink, std::mutex& sinkMutex) : sink_(sink), sinkMutex_(sinkMutex) {}
    aclptiResult Consume(const CommunicationBatch& result);
    aclptiResult Finish();
    uint64_t Count() const { return count_; }
    bool Complete() const { return complete_; }

private:
    void PublishFragment(const std::string& fragment, uint64_t records);
    void ConsumeRecords(const std::vector<aclpti::communication::Record>& records);
    std::string Manifest() const;
    ArtifactSink& sink_;
    std::mutex& sinkMutex_;
    uint64_t sequence_ = 0, count_ = 0;
    bool failed_ = false, complete_ = false, finished_ = false;
    aclptiResult status_ = ACLPTI_SUCCESS;
    uint64_t errors_ = 0, dropped_ = 0;
    std::string diagnostic_;
    std::vector<aclpti::communication::DeviceClocks> clocks_;
    std::string error_;
    std::vector<std::pair<std::string, uint64_t>> fragments_;
};
} // namespace npucompute
#endif
