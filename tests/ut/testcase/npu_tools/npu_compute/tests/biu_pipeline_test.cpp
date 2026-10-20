/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "biu/biu_pipeline.h"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint32_t Stamp(uint16_t value, uint8_t blockByte = 0)
{
    return 0xe0000000U | (static_cast<uint32_t>(blockByte) << 16U) | value;
}

std::vector<uint32_t> TraceWords(uint64_t timestamp, uint16_t blockId, uint16_t busyCycles)
{
    return {
        Stamp(static_cast<uint16_t>(timestamp), static_cast<uint8_t>(blockId)),
        Stamp(static_cast<uint16_t>(timestamp >> 16U), static_cast<uint8_t>(blockId >> 8U)),
        Stamp(static_cast<uint16_t>(timestamp >> 32U)),
        Stamp(static_cast<uint16_t>(timestamp >> 48U)),
        0xf0000000U,
        0xf001000aU,
        0x00030005U,
        0xf0000000U | busyCycles,
        0xefffffffU,
        0xddddddddU,
    };
}

uint32_t EventWord(uint8_t ctrl, uint16_t event, uint16_t cycles)
{
    return (static_cast<uint32_t>(ctrl & 0xfU) << 28U) | (static_cast<uint32_t>(event & 0xfffU) << 16U) | cycles;
}

uint32_t DfxWord(uint8_t ctrl, uint16_t regionId, uint16_t cycles)
{
    return (static_cast<uint32_t>(ctrl & 0xfU) << 28U) | (static_cast<uint32_t>(regionId & 0xfffU) << 16U) | cycles;
}

uint32_t StampWord(uint32_t value) { return 0xe0000000U | value; }

TEST(NpuComputeBiuPipeline, ParseIntervals)
{
    aclptiPipelineData replay;
    replay.deviceId = 0;
    replay.format = aclptiBiuFormat::Chip6;
    replay.channels[{0, aclptiBiuCoreKind::Aic}] = TraceWords(1000, 4, 15);
    replay.channels[{2, aclptiBiuCoreKind::Aiv0}] = TraceWords(1100, 9, 4);

    npucompute::BiuClockConfig clocks{100000000.0, 1000000000.0, 2000000000.0, "unit-test"};
    npucompute::BiuParseStats stats;
    std::vector<npucompute::BiuTraceEvent> intervals;
    const aclptiResult status = npucompute::ParsePipelineReplay(
        7, replay, clocks,
        [&intervals](const npucompute::BiuTraceEvent& interval) {
            intervals.push_back(interval);
            return ACLPTI_SUCCESS;
        },
        &stats);

    ASSERT_TRUE(status == ACLPTI_SUCCESS);
    std::vector<npucompute::BiuTraceEvent> busyIntervals;
    std::copy_if(intervals.begin(), intervals.end(), std::back_inserter(busyIntervals), [](const auto& event) {
        return !event.isDfx;
    });
    ASSERT_TRUE(busyIntervals.size() == 2);
    ASSERT_TRUE(busyIntervals[0].channel.groupId == 0);
    ASSERT_TRUE(busyIntervals[0].channel.coreKind == aclptiBiuCoreKind::Aic);
    ASSERT_TRUE(busyIntervals[0].name == "SCALAR");
    ASSERT_TRUE(busyIntervals[0].blockId == 4);
    ASSERT_TRUE(std::abs(busyIntervals[0].startUs - 0.01) < 1e-12);
    ASSERT_TRUE(std::abs(busyIntervals[0].durationUs - 0.02) < 1e-12);
    ASSERT_TRUE(busyIntervals[1].channel.groupId == 2);
    ASSERT_TRUE(std::abs(busyIntervals[1].startUs - 1.005) < 1e-12);
    ASSERT_TRUE(std::abs(busyIntervals[1].durationUs - 0.0045) < 1e-12);
    ASSERT_TRUE(stats.payloadBytes == 80);
    ASSERT_TRUE(stats.paddingBytes == 8);
    ASSERT_TRUE(stats.intervalCount == 2);
}

TEST(NpuComputeBiuPipeline, IncompleteIntervalsAreSkipped)
{
    aclptiPipelineData replay;
    replay.deviceId = 0;
    replay.format = aclptiBiuFormat::Chip6;
    // vector_add: unknown initial SCALAR start, complete MTE2, unclosed SCALAR tail.
    for (const auto core : {aclptiBiuCoreKind::Aiv0, aclptiBiuCoreKind::Aiv1}) {
        replay.channels[{0, core}] = {Stamp(1000), Stamp(0),    Stamp(0),    Stamp(0),
                                      0xf0010014U, 0xf01001b0U, 0xf00004daU, 0xf0010004U};
    }
    npucompute::BiuClockConfig clocks{1e9, 1e9, 1e9, "unit-test"};
    npucompute::BiuParseStats stats;
    const auto parse = [&]() {
        return npucompute::ParsePipelineReplay(1, replay, clocks, [](const auto&) { return ACLPTI_SUCCESS; }, &stats);
    };
    ASSERT_TRUE(parse() == ACLPTI_SUCCESS);
    ASSERT_TRUE(stats.intervalCount == 2);
    ASSERT_TRUE(stats.incompleteIntervalCount == 4);
    // A new anchor also ends an unclosed interval without losing later data.
    auto& words = replay.channels.begin()->second;
    const auto next = words;
    words.insert(words.end(), next.begin(), next.end());
    ASSERT_TRUE(parse() == ACLPTI_SUCCESS);
    ASSERT_TRUE(stats.intervalCount == 3);
    ASSERT_TRUE(stats.incompleteIntervalCount == 6);
    // A valid stream with only incomplete intervals still permits an empty trace.
    replay.channels.clear();
    replay.channels[{0, aclptiBiuCoreKind::Aiv0}] = {Stamp(1000), Stamp(0), Stamp(0), Stamp(0), 0xf0010014U};
    ASSERT_TRUE(parse() == ACLPTI_SUCCESS);
    ASSERT_TRUE(stats.intervalCount == 0);
    ASSERT_TRUE(stats.incompleteIntervalCount == 1);
}

TEST(NpuComputeBiuPipeline, DecodeFailures)
{
    aclptiPipelineData replay;
    replay.deviceId = 0;
    replay.format = aclptiBiuFormat::Chip6;
    replay.channels[{0, aclptiBiuCoreKind::Aic}] = {Stamp(1), Stamp(2)};
    npucompute::BiuClockConfig clocks{100000000.0, 1000000000.0, 1000000000.0, "unit-test"};
    npucompute::BiuParseStats stats;
    ASSERT_TRUE(
        npucompute::ParsePipelineReplay(
            1, replay, clocks, [](const auto&) { return ACLPTI_SUCCESS; }, &stats) == ACLPTI_ERROR_TRACE_INCOMPLETE);

    replay.channels[{0, aclptiBiuCoreKind::Aic}] = {0x70000000U};
    ASSERT_TRUE(
        npucompute::ParsePipelineReplay(
            1, replay, clocks, [](const auto&) { return ACLPTI_SUCCESS; }, &stats) == ACLPTI_ERROR_DECODE);
}

TEST(NpuComputeBiuPipeline, DfxEventsAndChip6Fixp)
{
    aclptiPipelineData replay;
    replay.deviceId = 0;
    replay.format = aclptiBiuFormat::Chip6;
    auto words = std::vector<uint32_t>{StampWord(0), StampWord(0), StampWord(0), StampWord(0)};
    // DFX trace start/end for region 7 on VECTOR.
    words.push_back(DfxWord(1, 0x407, 10));
    words.push_back(DfxWord(1, 0xc07, 15));
    // msopprof encodes FIXP DFX events with ctrl=10 (not Chip6 state bit 6).
    words.push_back(DfxWord(10, 0x40b, 10));
    words.push_back(DfxWord(10, 0xc0b, 15));
    words.push_back(0xefffffffU);
    replay.channels[{0, aclptiBiuCoreKind::Aic}] = words;

    auto markerWords = std::vector<uint32_t>{StampWord(0), StampWord(0), StampWord(0), StampWord(0)};
    // A user DFX mark before the internal tail sequence must remain visible.
    markerWords.push_back(DfxWord(2, 13, 1));
    for (const uint16_t marker : {0xd88, 0xd99, 0xdaa, 0xdbb, 0xdcc, 0xddd, 0xdee, 0xdff}) {
        markerWords.push_back(DfxWord(0, marker, 1));
    }
    // Kernel-end probe repeats the last marker after its drain delay to validate the sequence.
    markerWords.push_back(DfxWord(0, 0xdff, 3500));
    markerWords.push_back(0xefffffffU);
    replay.channels[{1, aclptiBiuCoreKind::Aic}] = markerWords;

    npucompute::BiuClockConfig clocks{1e9, 1e9, 1e9, "unit-test"};
    npucompute::BiuParseStats stats;
    std::vector<npucompute::BiuTraceEvent> events;
    const auto status = npucompute::ParsePipelineReplay(
        4, replay, clocks,
        [&events](const npucompute::BiuTraceEvent& event) {
            events.push_back(event);
            return ACLPTI_SUCCESS;
        },
        &stats);
    ASSERT_TRUE(status == ACLPTI_SUCCESS);
    ASSERT_TRUE(stats.intervalCount == 0);
    ASSERT_TRUE(stats.dfxEventCount == 7);
    ASSERT_TRUE(events.size() == 7);
    std::vector<std::string> names;
    for (const auto& event : events) {
        names.push_back(event.name);
    }
    ASSERT_TRUE(std::find(names.begin(), names.end(), "MarkStamp1031") != names.end());
    ASSERT_TRUE(std::find(names.begin(), names.end(), "MarkStamp3079") != names.end());
    ASSERT_TRUE(std::find(names.begin(), names.end(), "Region7") != names.end());
    ASSERT_TRUE(std::find(names.begin(), names.end(), "MarkStamp1035") != names.end());
    ASSERT_TRUE(std::find(names.begin(), names.end(), "MarkStamp3083") != names.end());
    ASSERT_TRUE(std::find(names.begin(), names.end(), "Region11") != names.end());
    ASSERT_TRUE(std::find(names.begin(), names.end(), "MarkStamp13") != names.end());
    ASSERT_TRUE(std::find(names.begin(), names.end(), "MarkStamp3464") == names.end());
    ASSERT_TRUE(std::find(names.begin(), names.end(), "MarkStamp3479") == names.end());
    const auto mark =
        std::find_if(events.begin(), events.end(), [](const auto& event) { return event.name == "MarkStamp1031"; });
    ASSERT_TRUE(mark != events.end());
    ASSERT_TRUE(mark->pipe == npucompute::BiuPipe::Vector);
    ASSERT_TRUE(std::abs(mark->durationUs - 0.001) < 1e-12);
    const auto region =
        std::find_if(events.begin(), events.end(), [](const auto& event) { return event.name == "Region7"; });
    ASSERT_TRUE(region != events.end());
    ASSERT_TRUE(region->pipe == npucompute::BiuPipe::Vector);
    ASSERT_TRUE(std::abs(region->durationUs - 0.015) < 1e-12);
    const auto fixpDfxMark =
        std::find_if(events.begin(), events.end(), [](const auto& event) { return event.name == "MarkStamp1035"; });
    ASSERT_TRUE(fixpDfxMark != events.end());
    ASSERT_TRUE(fixpDfxMark->pipe == npucompute::BiuPipe::Fixp);
    const auto fixpDfxRegion =
        std::find_if(events.begin(), events.end(), [](const auto& event) { return event.name == "Region11"; });
    ASSERT_TRUE(fixpDfxRegion != events.end());
    ASSERT_TRUE(fixpDfxRegion->pipe == npucompute::BiuPipe::Fixp);
    ASSERT_TRUE(std::abs(fixpDfxRegion->durationUs - 0.015) < 1e-12);
}

TEST(NpuComputeBiuPipeline, IncompleteEndMarkSequenceKeepsDfxEvents)
{
    aclptiPipelineData replay;
    replay.deviceId = 0;
    replay.format = aclptiBiuFormat::Chip6;
    auto words = std::vector<uint32_t>{StampWord(0), StampWord(0), StampWord(0), StampWord(0)};
    words.push_back(DfxWord(0, 0xd88, 1));
    words.push_back(DfxWord(0, 0xd99, 1));
    // A normal user mark invalidates the candidate end-mark sequence.
    words.push_back(DfxWord(1, 27, 1));
    words.push_back(0xefffffffU);
    replay.channels[{0, aclptiBiuCoreKind::Aic}] = words;

    npucompute::BiuClockConfig clocks{1e9, 1e9, 1e9, "unit-test"};
    npucompute::BiuParseStats stats;
    std::vector<npucompute::BiuTraceEvent> events;
    ASSERT_TRUE(
        npucompute::ParsePipelineReplay(
            5, replay, clocks,
            [&events](const npucompute::BiuTraceEvent& event) {
                events.push_back(event);
                return ACLPTI_SUCCESS;
            },
            &stats) == ACLPTI_SUCCESS);
    ASSERT_TRUE(stats.dfxEventCount == 3);
    ASSERT_TRUE(events.size() == 3);
    ASSERT_TRUE(std::find_if(events.begin(), events.end(), [](const auto& event) {
                    return event.name == "MarkStamp3464";
                }) != events.end());
    ASSERT_TRUE(std::find_if(events.begin(), events.end(), [](const auto& event) {
                    return event.name == "MarkStamp3481";
                }) != events.end());
    ASSERT_TRUE(std::find_if(events.begin(), events.end(), [](const auto& event) {
                    return event.name == "MarkStamp27";
                }) != events.end());
}

TEST(NpuComputeBiuPipeline, OnlyRequiresUsedCoreClock)
{
    aclptiPipelineData replay;
    replay.deviceId = 0;
    replay.format = aclptiBiuFormat::Chip6;
    replay.channels[{0, aclptiBiuCoreKind::Aic}] = TraceWords(1000, 4, 15);

    npucompute::BiuClockConfig clocks{100000000.0, 1000000000.0, 0.0, "unit-test"};
    npucompute::BiuParseStats stats;
    ASSERT_TRUE(
        npucompute::ParsePipelineReplay(
            1, replay, clocks, [](const auto&) { return ACLPTI_SUCCESS; }, &stats) == ACLPTI_SUCCESS);

    replay.channels.clear();
    replay.channels[{0, aclptiBiuCoreKind::Aiv0}] = TraceWords(1000, 4, 15);
    ASSERT_TRUE(
        npucompute::ParsePipelineReplay(
            1, replay, clocks, [](const auto&) { return ACLPTI_SUCCESS; }, &stats) == ACLPTI_ERROR_RESULT_UNRELIABLE);
}

TEST(NpuComputeBiuPipeline, PipeTraceWriter)
{
    const boost::filesystem::path directory =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("npu-compute-biu-%%%%-%%%%");
    ASSERT_TRUE(boost::filesystem::create_directory(directory));

    npucompute::PipeTraceWriter writer;
    std::string error;
    const aclptiResult beginStatus = writer.Begin(directory / "PipeTrace.json", &error);
    if (beginStatus != ACLPTI_SUCCESS) {
        std::fprintf(stderr, "PipeTraceWriter::Begin failed: %d %s\n", beginStatus, error.c_str());
    }
    ASSERT_TRUE(beginStatus == ACLPTI_SUCCESS);
    npucompute::BiuTraceEvent event{
        {7, 0, 3, aclptiBiuCoreKind::Aiv1}, npucompute::BiuPipe::Vector, 5, "VECTOR", "rail_idle", 1.25, 0.5, false};
    ASSERT_TRUE(writer.Append(event) == ACLPTI_SUCCESS);
    ASSERT_TRUE(writer.Commit() == ACLPTI_SUCCESS);

    std::ifstream input((directory / "PipeTrace.json").string());
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ASSERT_TRUE(json.find("\"displayTimeUnit\":\"ns\"") != std::string::npos);
    ASSERT_TRUE(json.find("\"name\":\"VECTOR\"") != std::string::npos);
    ASSERT_TRUE(json.find("\"pid\":\"group3.veccore1\"") != std::string::npos);
    ASSERT_TRUE(json.find("\"tid\":\"VECTOR\"") != std::string::npos);
    ASSERT_TRUE(json.find("\"ph\":\"X\"") != std::string::npos);

    npucompute::PipeTraceWriter regionWriter;
    ASSERT_TRUE(regionWriter.Begin(directory / "region.json", &error) == ACLPTI_SUCCESS);
    npucompute::BiuTraceEvent regionEvent{
        {7, 0, 3, aclptiBiuCoreKind::Aiv1}, npucompute::BiuPipe::Vector, 5, "Region10", "good", 1.25, 0.5, true};
    ASSERT_TRUE(regionWriter.Append(regionEvent) == ACLPTI_SUCCESS);
    ASSERT_TRUE(regionWriter.Commit() == ACLPTI_SUCCESS);
    std::ifstream regionInput((directory / "region.json").string());
    const std::string regionJson((std::istreambuf_iterator<char>(regionInput)), std::istreambuf_iterator<char>());
    ASSERT_TRUE(regionJson.find("\"cname\":\"good\"") != std::string::npos);
    ASSERT_TRUE(regionJson.find("\"cname\":\"orange\"") == std::string::npos);

    npucompute::PipeTraceWriter emptyWriter;
    ASSERT_TRUE(emptyWriter.Begin(directory / "empty.json", &error) == ACLPTI_SUCCESS);
    ASSERT_TRUE(emptyWriter.Commit() == ACLPTI_SUCCESS);
    boost::system::error_code ignored;
    boost::filesystem::remove_all(directory, ignored);
}

} // namespace
