/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_DATA_TYPES_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_DATA_TYPES_H

#include "aclpti/aclpti_data.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <variant>

namespace aclpti::data {

inline constexpr std::size_t kMaxPmuSlots = 10;
inline constexpr uint32_t kInvalidPmuEvent = 0xffffffffU;
// Hardware slot order matters; unused slots form a suffix filled with kInvalidPmuEvent.
using PmuSlots = std::array<uint32_t, kMaxPmuSlots>;

enum class ReplayKind { Pmu, Pipeline, PcSampling };

// Device and BIU format are learned from received data, not supplied by the caller.
struct ReplayPrepareInfo {
    uint64_t replayId;
    PmuSlots pmuEventIds;
    ReplayKind kind = ReplayKind::Pmu;
    std::shared_ptr<const aclptiPmSamplingContext> pmSamplingContext;
};

struct ReplayStopInfo {
    uint64_t replayId;
    aclptiResult stopStatus;
};

// Reception diagnostics include task logs as well as the replay's primary data type.
// copied* counts successful queue submissions; receivedBytes counts fully accepted callbacks.
struct CallbackStats {
    uint64_t copiedRecordCount;
    uint64_t copiedBytes;
    uint64_t receivedBytes;
};

// Lightweight stop result; bulk profiling data is delivered separately at manager shutdown.
struct ReplayResult {
    uint64_t replayId;
    aclptiResult status;
    CallbackStats callbackStats;
};

constexpr uint8_t kBlockPmuFunctionType = 0x29U;
constexpr uint8_t kTaskPmuFunctionType = 0x2aU;

// Decoded fields, not a packed representation of the 32-byte wire record.
struct TaskLog32 {
    uint8_t funcType;
    uint16_t taskId;
    uint16_t rtStreamId;
    uint64_t systemCounter;
    uint16_t blockId;
    uint16_t subBlockId;
    aclptiCoreType coreType;
    uint8_t coreTypeId;
};

// Counter values stay in hardware units here; frequency conversion occurs downstream.
struct PmuRecord128 {
    uint8_t funcType;
    uint16_t taskId;
    uint16_t rtStreamId;
    uint64_t totalCycles;
    uint64_t taskStartSystemCounter;
    uint64_t taskEndSystemCounter;
    bool overflow;
    aclptiCoreType coreType;
    uint8_t coreId;
    uint16_t blockId;
    uint16_t subBlockId;
    std::map<uint32_t, double> pmuValues;
};

struct DecodedRecord {
    uint64_t recordIndex;
    std::variant<TaskLog32, PmuRecord128> payload;
};

class DecodeResult {
public:
    explicit DecodeResult(aclptiResult status) : status_(status) {}
    explicit DecodeResult(DecodedRecord value) : value_(std::move(value)) {}

    bool Ok() const { return status_ == ACLPTI_SUCCESS && value_.has_value(); }
    aclptiResult Status() const { return status_; }
    const DecodedRecord& Value() const { return value_.value(); }

private:
    aclptiResult status_ = ACLPTI_SUCCESS;
    std::optional<DecodedRecord> value_;
};

} // namespace aclpti::data

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_DATA_TYPES_H
