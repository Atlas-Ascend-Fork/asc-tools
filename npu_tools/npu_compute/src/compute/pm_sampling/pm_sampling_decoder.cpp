/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "pm_sampling_decoder.h"
#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace npucompute {
namespace {
static_assert(
    kPmSamplingStateOffset + kPmSamplingRawStateCount <= kPmSamplingRecordBytes,
    "PmSampling record no longer contains all wire states");
} // namespace

bool AddPmSamplingStates(PmSamplingStateCounts& destination, const PmSamplingStateCounts& source)
{
    for (std::size_t state = 0; state < destination.size(); ++state) {
        if (source[state] > std::numeric_limits<std::uint64_t>::max() - destination[state])
            return false;
    }
    for (std::size_t state = 0; state < destination.size(); ++state)
        destination[state] += source[state];
    return true;
}

PmSamplingDecoder::PmSamplingDecoder(std::uint64_t replayId, std::shared_ptr<const aclptiPmSamplingContext> context)
    : replayId_(replayId), context_(std::move(context))
{
    if (context_ && !context_->instructionMapError.empty()) {
        Fail(context_->instructionMapError.c_str());
        return;
    }
    if (!context_ || !context_->companionElf || context_->companionElf->empty() || !context_->businessInstructions ||
        context_->businessInstructions->empty() || !context_->excludedInstructions ||
        context_->elfEntry > kPmSamplingPcMask || (!context_->aicRuntimeEntry && !context_->aivRuntimeEntry) ||
        (context_->aicRuntimeEntry && context_->aicRuntimeEntry < context_->elfEntry) ||
        (context_->aivRuntimeEntry && context_->aivRuntimeEntry < context_->elfEntry)) {
        Fail("invalid PmSampling context");
        return;
    }
    for (const auto& item : *context_->businessInstructions) {
        if (item.first > kPmSamplingPcMask || context_->excludedInstructions->count(item.first)) {
            Fail("overlapping or unsupported PmSampling instruction address");
            return;
        }
    }
    for (const auto pc : *context_->excludedInstructions) {
        if (pc > kPmSamplingPcMask) {
            Fail("unsupported PmSampling instruction address");
            return;
        }
    }
}

bool PmSamplingDecoder::Fail(const char* message)
{
    if (error_.empty())
        error_ = message;
    result_ = {};
    return false;
}

bool PmSamplingDecoder::Decode(const std::array<std::uint8_t, kPmSamplingRecordBytes>& record)
{
    // Encoded bits [23:0] represent runtime address bits [26:3]. Subtract the
    // image load base modulo 2^27, then verify the candidate against known instructions.
    std::uint64_t encodedPc = 0;
    for (std::size_t byte = 0; byte < kPmSamplingEncodedPcBytes; ++byte) {
        encodedPc |= std::uint64_t(record[byte]) << (kPmSamplingByteBits * byte);
    }
    const auto rawPc = encodedPc * kPmSamplingAddressScale;
    std::optional<std::uint64_t> selected;
    for (const auto entry : {context_->aicRuntimeEntry, context_->aivRuntimeEntry}) {
        if (!entry)
            continue;
        const auto pc = (rawPc - ((entry - context_->elfEntry) & kPmSamplingPcMask)) & kPmSamplingPcMask;
        if (!context_->businessInstructions->count(pc) && !context_->excludedInstructions->count(pc))
            continue;
        if (selected && *selected != pc)
            return Fail("ambiguous AIC/AIV sampling PC");
        selected = pc;
    }
    if (!selected)
        return Fail("unknown sampling PC");
    ++result_.decodedRecords;
    if (context_->excludedInstructions->count(*selected)) {
        ++result_.excludedRecords;
        return true;
    }
    PmSamplingStateCounts counts{};
    for (std::size_t state = 0; state < counts.size(); ++state)
        counts[state] = record[kPmSamplingStateOffset + state];
    // Byte 11 is Active; skip it without storing or accumulating it. Consume
    // still advances over the complete 16-byte record, including reserved bytes.
    const auto originalPc = context_->businessInstructions->at(*selected);
    if (!AddPmSamplingStates(result_.rows[originalPc], counts))
        return Fail("PmSampling state count overflow");
    ++result_.businessRecords;
    // Bytes 12..15 are reserved; do not infer a Core ID or reject future nonzero flags.
    return true;
}

bool PmSamplingDecoder::Consume(const aclptiRawDataChunk& chunk)
{
    if (!error_.empty())
        return false;
    if (finished_)
        return Fail("PmSampling decoder already finished");
    if (chunk.replayId != replayId_ || chunk.deviceId < 0 || chunk.chunkModule < 0 || chunk.bytes.empty() ||
        (deviceId_ != -1 && deviceId_ != chunk.deviceId))
        return Fail("invalid PmSampling chunk identity");
    if (chunk.bytes.size() > std::numeric_limits<std::uint64_t>::max() - result_.receivedBytes)
        return Fail("PmSampling received byte count overflow");
    try {
        auto& channel = channels_[chunk.chunkModule];
        if (chunk.offset != channel.offset ||
            chunk.bytes.size() > std::numeric_limits<std::size_t>::max() - channel.offset)
            return Fail("discontinuous PmSampling chunk offset");
        deviceId_ = chunk.deviceId;
        result_.receivedBytes += chunk.bytes.size();
        channel.open = true;
        for (const auto byte : chunk.bytes) {
            channel.pending[channel.pendingSize++] = byte;
            if (channel.pendingSize == channel.pending.size()) {
                if (!Decode(channel.pending))
                    return false;
                channel.pendingSize = 0;
            }
        }
        channel.offset += chunk.bytes.size();
        if (chunk.isLastChunk) {
            if (channel.pendingSize)
                return Fail("truncated PmSampling packet record");
            channel.offset = 0;
            channel.open = false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return Fail("PmSampling allocation failed");
    }
}

bool PmSamplingDecoder::Finish(PmSamplingResult& output)
{
    output = {};
    if (!error_.empty())
        return false;
    if (finished_)
        return Fail("PmSampling decoder already finished");
    for (const auto& item : channels_) {
        if (item.second.open || item.second.pendingSize)
            return Fail("unfinished PmSampling packet");
    }
    finished_ = true;
    output = std::move(result_);
    return true;
}
} // namespace npucompute
