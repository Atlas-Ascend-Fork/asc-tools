/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_DECODER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_DECODER_H

#include "aclpti/aclpti_data.h"
#include <map>
#include <cstddef>
#include <cstdint>
#include "pm_sampling_types.h"
#include <array>
#include <memory>
#include <string>

namespace npucompute {
// Decodes the 16-byte callback format verified on Ascend950.
class PmSamplingDecoder {
public:
    explicit PmSamplingDecoder(std::uint64_t replayId, std::shared_ptr<const aclptiPmSamplingContext> context);
    bool Consume(const aclptiRawDataChunk& chunk);
    // Only Finish exposes rows; errors invalidate all partial results.
    bool Finish(PmSamplingResult& output);
    const std::string& Error() const { return error_; }

private:
    struct Channel {
        std::size_t offset = 0;
        std::array<std::uint8_t, kPmSamplingRecordBytes> pending{};
        std::size_t pendingSize = 0;
        bool open = false;
    };
    bool Decode(const std::array<std::uint8_t, kPmSamplingRecordBytes>& record);
    bool Fail(const char* message);
    const std::uint64_t replayId_;
    const std::shared_ptr<const aclptiPmSamplingContext> context_;
    std::map<std::int32_t, Channel> channels_;
    PmSamplingResult result_;
    std::int32_t deviceId_ = -1;
    std::string error_;
    bool finished_ = false;
};
} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_DECODER_H
