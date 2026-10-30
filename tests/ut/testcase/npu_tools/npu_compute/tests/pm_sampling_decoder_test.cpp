/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "pm_sampling/pm_sampling_decoder.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>
#include <cstdio>
#include <limits>
#include <memory>
#include <set>
#include <string>

#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
            return 1;                                            \
        }                                                        \
    } while (false)
using npucompute::AddPmSamplingStates;
using npucompute::kPmSamplingActiveOffset;
using npucompute::kPmSamplingAddressScale;
using npucompute::kPmSamplingByteBits;
using npucompute::kPmSamplingEncodedPcBytes;
using npucompute::kPmSamplingPcMask;
using npucompute::kPmSamplingRecordBytes;
using npucompute::kPmSamplingStateOffset;
using npucompute::PmSamplingDecoder;
using npucompute::PmSamplingResult;
using npucompute::PmSamplingStateCounts;
namespace {
std::shared_ptr<aclptiPmSamplingContext> Context()
{
    auto context = std::make_shared<aclptiPmSamplingContext>();
    context->companionElf = std::make_shared<const std::vector<char>>(1, 'E');
    context->businessInstructions = std::make_shared<const std::map<uint64_t, uint64_t>>(
        std::map<uint64_t, uint64_t>{{0, 0}, {0x10, 0x10}, {0x18, 0x18}});
    context->simtInstructions = std::make_shared<const std::set<uint64_t>>(std::set<uint64_t>{0, 0x10, 0x18});
    context->elfEntry = 0x100;
    context->aivRuntimeEntry = 0x120000100000;
    context->pcOffset = 0xf0;
    return context;
}
std::vector<uint8_t> Record(uint64_t pc, const PmSamplingStateCounts& states = {}, uint8_t active = 0)
{
    const auto encoded = ((0x100000 + pc) & kPmSamplingPcMask) / kPmSamplingAddressScale;
    std::vector<uint8_t> bytes(kPmSamplingRecordBytes);
    for (unsigned i = 0; i < kPmSamplingEncodedPcBytes; ++i) {
        bytes[i] = (encoded >> (kPmSamplingByteBits * i)) & 0xff;
    }
    for (unsigned i = 0; i < states.size(); ++i) {
        bytes[kPmSamplingStateOffset + i] = states[i];
    }
    bytes[kPmSamplingActiveOffset] = active;
    return bytes;
}
aclptiRawDataChunk Chunk(std::vector<uint8_t> bytes, int module = 3, size_t offset = 0, bool last = true)
{
    return {7, 0, module, offset, last, std::move(bytes)};
}
int Aggregation()
{
    PmSamplingDecoder decoder(7, Context());
    CHECK(decoder.Consume(Chunk(Record(0x100, {1, 0, 4, 0, 0, 0, 0, 0}, 5))));
    CHECK(decoder.Consume(Chunk(Record(0x100, {44, 47, 123, 0, 21, 0, 0, 0}, 10), 4)));
    CHECK(decoder.Consume(Chunk(Record(0xf0, {2, 0, 0, 0, 0, 0, 0, 0}, 1)))); // Original PC zero.
    CHECK(decoder.Consume(Chunk(Record(0x108))));                             // All-zero business row survives.
    CHECK(decoder.Consume(Chunk(Record(0x120, {255, 255, 255}))));            // Unmapped runtime PC.
    CHECK(decoder.Consume(Chunk(Record(0x110, {255, 255, 255}))));            // Probe.
    CHECK(decoder.Consume(Chunk(Record(0x118, {255, 255, 255}))));            // Added ABI instruction.
    PmSamplingResult result;
    CHECK(decoder.Finish(result));
    CHECK(result.rows.size() == 3 && !result.rows.count(0x110) && !result.rows.count(0x118));
    CHECK(result.rows.at(0x10) == PmSamplingStateCounts({45, 47, 127, 0, 21, 0, 0, 0}));
    CHECK(result.rows.at(0x18) == PmSamplingStateCounts{});
    CHECK(result.rows.count(0) == 1 && result.rows.count(0x100) == 0);
    CHECK(result.receivedBytes == 7 * kPmSamplingRecordBytes);
    std::puts("synthetic: states=[45,47,127,0,21,0,0,0], Active ignored, unmapped PCs skipped");
    return 0;
}
int Fragments()
{
    // Every possible byte boundary, including boundaries within the 3-byte PC and a state.
    for (size_t split = 1; split < kPmSamplingRecordBytes; ++split) {
        PmSamplingDecoder decoder(7, Context());
        const auto bytes = Record(0x100, {1, 2, 3, 4, 5, 6, 7, 8}, 9);
        CHECK(decoder.Consume(Chunk({bytes.begin(), bytes.begin() + split}, 3, 0, false)));
        CHECK(decoder.Consume(Chunk(Record(0x100, {1}), 4)));
        CHECK(decoder.Consume(Chunk({bytes.begin() + split, bytes.end()}, 3, split)));
        CHECK(decoder.Consume(Chunk(bytes))); // New packet restarts offset zero.
        PmSamplingResult result;
        CHECK(decoder.Finish(result));
        CHECK(result.rows.at(0x10) == PmSamplingStateCounts({3, 4, 6, 8, 10, 12, 14, 16}));
    }
    return 0;
}
int Failures()
{
    for (int scenario = 0; scenario < 8; ++scenario) {
        auto context = Context();
        if (scenario == 6) {
            context->aicRuntimeEntry = context->aivRuntimeEntry;
        }
        PmSamplingDecoder decoder(7, context);
        CHECK(decoder.Consume(Chunk(Record(0x100))));
        auto bad = Chunk(Record(0x108));
        if (scenario == 0) {
            bad.offset = 1;
        }
        if (scenario == 1) {
            bad.replayId = 8;
        }
        if (scenario == 2) {
            bad.deviceId = 1;
        }
        if (scenario == 3) {
            bad.bytes.resize(15);
        }
        if (scenario == 4) {
            bad.bytes = Record(0x120);
        }
        if (scenario == 7) {
            bad.isLastChunk = false;
        }
        if (scenario == 4 || scenario == 5 || scenario == 6) {
            CHECK(decoder.Consume(bad));
            PmSamplingResult result;
            CHECK(decoder.Finish(result)); // Repeated packets and identical AIC/AIV bases are allowed.
        } else if (scenario == 7) {
            CHECK(decoder.Consume(bad));
            PmSamplingResult result;
            CHECK(!decoder.Finish(result));
            CHECK(result.rows.empty());
        } else {
            CHECK(!decoder.Consume(bad));
            CHECK(!decoder.Error().empty());
            PmSamplingResult result;
            result.rows[0x100] = {};
            CHECK(!decoder.Finish(result) && result.rows.empty());
        }
    }
    return 0;
}
int AddressesAndLimits()
{
    auto context = Context();
    context->aicRuntimeEntry = context->aivRuntimeEntry - 8;
    PmSamplingDecoder ambiguous(7, context);
    CHECK(!ambiguous.Consume(Chunk(Record(0x100))));
    CHECK(ambiguous.Error().find("ambiguous") != std::string::npos);
    context = Context();
    context->aicRuntimeEntry = context->aivRuntimeEntry + 0x1000;
    PmSamplingDecoder unique(7, context);
    CHECK(unique.Consume(Chunk(Record(0x100))));
    PmSamplingResult result;
    CHECK(unique.Finish(result));
    context = Context();
    context->aivRuntimeEntry = 0x120007ffff00; // Sampling crosses the 27-bit address wrap.
    PmSamplingDecoder wrap(7, context);
    auto bytes = Record(0x100);
    bytes[0] = bytes[1] = bytes[2] = 0;
    CHECK(wrap.Consume(Chunk(bytes)));
    CHECK(wrap.Finish(result));
    CHECK(result.rows.count(0x10));
    context = Context();
    context->originalEntry = 0x10;
    context->pcOffset = 0x100;
    PmSamplingDecoder nonzeroEntry(7, context);
    CHECK(nonzeroEntry.Consume(Chunk(Record(0x100, {7}))));
    CHECK(nonzeroEntry.Finish(result));
    CHECK(result.rows.at(0x10)[0] == 7);
    context = Context();
    context->aivRuntimeEntry += 0xf0;
    context->pcOffset = 0;
    PmSamplingDecoder zeroOffset(7, context);
    CHECK(zeroOffset.Consume(Chunk(Record(0x100, {9}))));
    CHECK(zeroOffset.Finish(result));
    CHECK(result.rows.at(0x10)[0] == 9);
    context = Context();
    context->simtInstructions = std::make_shared<const std::set<uint64_t>>(std::set<uint64_t>{0x200});
    PmSamplingDecoder invalidSimtMap(7, context);
    CHECK(!invalidSimtMap.Consume(Chunk(Record(0x100))));
    PmSamplingDecoder invalid(7, nullptr);
    CHECK(!invalid.Finish(result));
    context = Context();
    context->aicRuntimeEntry = context->aivRuntimeEntry;
    context->aivRuntimeEntry = 0;
    PmSamplingDecoder aicOnly(7, context);
    auto reserved = Record(0x100);
    reserved[12] = 0xff;
    CHECK(aicOnly.Consume(Chunk(reserved)));
    CHECK(aicOnly.Finish(result));
    PmSamplingDecoder probes(7, Context());
    CHECK(probes.Consume(Chunk(Record(0x110, {255}))));
    CHECK(probes.Finish(result));
    CHECK(result.rows.empty());
    for (int scenario = 0; scenario < 3; ++scenario) {
        PmSamplingDecoder badIdentity(7, Context());
        auto chunk = Chunk(Record(0x100));
        if (scenario == 0) {
            chunk.deviceId = -1;
        }
        if (scenario == 1) {
            chunk.chunkModule = -1;
        }
        if (scenario == 2) {
            chunk.bytes.clear();
        }
        CHECK(!badIdentity.Consume(chunk));
        CHECK(!badIdentity.Finish(result));
    }
    PmSamplingStateCounts destination{};
    destination[7] = std::numeric_limits<uint64_t>::max();
    const auto original = destination;
    CHECK(!AddPmSamplingStates(destination, {1, 0, 0, 0, 0, 0, 0, 1}));
    CHECK(destination == original);
    destination = {};
    destination[0] = std::numeric_limits<uint64_t>::max() - 1;
    CHECK(AddPmSamplingStates(destination, {1}));
    CHECK(destination[0] == std::numeric_limits<uint64_t>::max());
    PmSamplingDecoder channels(7, Context());
    // Cross the former arbitrary 256-channel boundary and retain every record.
    constexpr int kChannelCount = 257;
    for (int module = 0; module < kChannelCount; ++module) {
        CHECK(channels.Consume(Chunk(Record(0x100, {1}), module)));
    }
    CHECK(channels.Finish(result));
    CHECK(result.rows.at(0x10)[0] == kChannelCount);
    context = Context();
    context->instructionMapError = "ambiguous business instruction alignment in kernel";
    PmSamplingDecoder mapFailure(7, context);
    CHECK(!mapFailure.Finish(result));
    CHECK(mapFailure.Error() == context->instructionMapError && result.rows.empty());
    context = Context();
    context->simtInstructions = std::make_shared<const std::set<uint64_t>>(std::set<uint64_t>{0x10});
    PmSamplingDecoder simd(7, context);
    CHECK(simd.Consume(Chunk(Record(0x108, {99}))));
    CHECK(simd.Finish(result));
    CHECK(result.rows.empty());
    return 0;
}
int RealGatherCallback()
{
    // 8 complete 128-byte packets (64 records), captured from official gather_2d
    // with the real tail probe on Ascend950/CANN 9.3.0. Embedded to avoid large fixture dependencies.
    const std::string hex = "8b20fc0003ff000000000000000000008b20fc0000ff00000000000000000000"
                            "8b20fc0006ff000000000000000000008b20fc0003ff00000000000000000000"
                            "8b20fc0006ff000000000000000000008b20fc0004ff00000000000000000000"
                            "8b20fc0002ff000000000000000000008b20fc0003ff00000000000000000000"
                            "8b20fc0000ff000000000000000000008b20fc0000ff00000000000000000000"
                            "8b20fc0005ff000000000000000000008b20fc0005ff00000000000000000000"
                            "8b20fc0003ff000000000000000000008b20fc0004ff00000000000100000000"
                            "8b20fc0006ff000000000000000000008b20fc0005ff00000000000000000000"
                            "8b20fc0000ff000000000000000000008b20fc0001ff00000000000000000000"
                            "8b20fc0005ff000000000000000000008b20fc0003ff00000000000000000000"
                            "8b20fc0004ff000000000000000000008b20fc0002ff00000000000000000000"
                            "8b20fc0003ff000000000000000000008b20fc0009ff00000000000200000000"
                            "8b20fc0002ff000000000000000000008b20fc0002ff00000000000000000000"
                            "8b20fc0004ff000000000001000000008b20fc0005ff00000000000100000000"
                            "8b20fc0005ff000000000000000000008b20fc0006ff00000000000000000000"
                            "8b20fc0004ff000000000001000000008b20fc0004ff00000000000000000000"
                            "8b20fc0000ff000000000000000000008b20fc0000ff00000000000200000000"
                            "8b20fc0004ff000000000000000000008b20fc0005ff00000000000000000000"
                            "8b20fc0005ff000000000000000000008b20fc0005ff00000000000000000000"
                            "8b20fc0005ff000000000000000000008b20fc0005ff00000000000000000000"
                            "8b20fc0001ff000000000000000000008b20fc0000ff00000000000000000000"
                            "8b20fc0004ff000000000000000000008b20fc0005ff00000000000000000000"
                            "8b20fc0003ff000000000000000000008b20fc0002ff00000000000000000000"
                            "8b20fc0004ff000000000001000000008b20fc0002ff00000000000000000000"
                            "8b20fc0002ff000000000000000000008b20fc0001ff00000000000000000000"
                            "8b20fc0002ff000000000000000000008b20fc0001ff00000000000000000000"
                            "8b20fc0005ff000000000000000000008b20fc0005ff00000000000000000000"
                            "8b20fc0003ff000000000000000000008b20fc0003ff00000000000200000000"
                            "8b20fc0000ff000000000000000000008b20fc0000ff00000000000000000000"
                            "8b20fc0006ff000000000000000000008b20fc0006ff00000000000000000000"
                            "8b20fc0003ff000000000000000000008b20fc0005ff00000000000200000000"
                            "8b20fc0004ff000000000000000000008b20fc0005ff00000000000000000000";
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.size(); i += 2) {
        bytes.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
    }
    auto context = Context();
    context->elfEntry = 0xff40;
    context->aivRuntimeEntry = 0x12004fe0ff40;
    context->pcOffset = 0x2b0;
    context->businessInstructions =
        std::make_shared<const std::map<uint64_t, uint64_t>>(std::map<uint64_t, uint64_t>{{0x268, 0x268}});
    context->simtInstructions = std::make_shared<const std::set<uint64_t>>(std::set<uint64_t>{0x268});
    PmSamplingDecoder decoder(7, context);
    for (size_t begin = 0; begin < bytes.size(); begin += 128) {
        CHECK(decoder.Consume(Chunk({bytes.begin() + begin, bytes.begin() + begin + 128})));
    }
    PmSamplingResult result;
    CHECK(decoder.Finish(result));
    CHECK(result.rows.size() == 1);
    CHECK(result.rows.at(0x268) == PmSamplingStateCounts({0, 215, 16320, 0, 0, 0, 0, 0}));
    std::puts("real gather: companion PC=0x10458 -> original PC=0x268 states=[0,215,16320,0,0,0,0,0], "
              "Active ignored");
    return 0;
}

} // namespace
int main()
{
    CHECK(Aggregation() == 0);
    CHECK(Fragments() == 0);
    CHECK(Failures() == 0);
    CHECK(AddressesAndLimits() == 0);
    CHECK(RealGatherCallback() == 0);
    std::puts("PASS: PmSampling decoding, filtering, aggregation and fail-closed validation");
}
