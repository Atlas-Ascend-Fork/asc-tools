/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef ACLPTI_COMMUNICATION_DECODER_H
#define ACLPTI_COMMUNICATION_DECODER_H
#include "aclpti/aclpti_communication.h"
#include "npu_compute/common.h"
#include "profiling/prof_common.h"
#include <map>
#include <string>
#include <tuple>
#include <vector>
namespace aclpti::communication {
// Single-owner streaming decoder. Finish validates the entire session, not individual packages.
class NPU_COMPUTE_EXPORT Decoder final {
public:
    bool Feed(const MsprofRawData& raw, std::vector<Record>& records);
    bool Finish();
    const std::string& Error() const { return error_; }

private:
    struct Stream {
        std::vector<uint8_t> bytes;
        size_t offset = 0;
        bool open = false;
        bool ignoredAdditionalPackage = false;
    };
    bool Fail(const char* message);
    bool Decode(
        const uint8_t* bytes, size_t size, int32_t rawType, const MsprofRawData& raw, std::vector<Record>& records);
    bool DecodeCompactRecord(const uint8_t* bytes, size_t size, Record r, std::vector<Record>& records);
    bool DecodeAdditionalRecord(const uint8_t* bytes, size_t size, Record r, std::vector<Record>& records);
    bool FeedAdditional(Stream& stream, const MsprofRawData& raw, std::vector<Record>& records);
    bool DecodeAdditionalPackage(const Stream& stream, const MsprofRawData& raw, std::vector<Record>& records);
    bool ReadRawType(const MsprofRawData& raw, int32_t& type);
    Stream* PrepareStream(const MsprofRawData& raw, int32_t type);
    bool BufferRaw(Stream& stream, const MsprofRawData& raw);
    bool DecodeBuffered(Stream& stream, int32_t type, const MsprofRawData& raw, std::vector<Record>& records);
    std::map<std::tuple<int32_t, int32_t, int32_t>, Stream> streams_;
    std::string error_;
};
} // namespace aclpti::communication
#endif
