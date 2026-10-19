/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/racecheck.h"
#include "aclsan_device_data.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include <gtest/gtest.h>

namespace {
using namespace aclsan;
TEST(RacecheckTranslation, UnitFlagModesTravelWithMemoryBehavior)
{
    for (uint32_t mode : {0, 2, 3}) {
        for (uint32_t id : {400, 91, 168, 170}) {
            ParsedTraceRecord raw{};
            raw.launchId = 1;
            raw.instrExecId = 1;
            raw.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
            raw.record.instrId = id;
            raw.record.pipeline = id == 400 ? ACLSAN_DEVICE_PIPE_MATRIX : ACLSAN_DEVICE_PIPE_FIXPIPE;
            if (id == 400) {
                raw.record.args[3] = 16 | (64ULL << 12) | (16ULL << 24) | (1ULL << 63) | (uint64_t(mode) << 55);
            } else {
                raw.record.args[0] = 0x8000;
                raw.record.args[2] = (16ULL << 4) | (16ULL << 16) | (256ULL << 32);
                raw.record.args[3] = 16 | (uint64_t(mode) << 32);
            }
            auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw.record);
            ASSERT_TRUE(decoded.has_value());
            auto translated = TranslateDecodedTraceToCallbackData(raw, *decoded, MemoryRegisterState{});
            ASSERT_TRUE(translated.has_value());
            const auto* accesses = std::get_if<DeviceMemoryAccessDataList>(&*translated);
            ASSERT_NE(accesses, nullptr);
            ASSERT_FALSE(accesses->empty());
            bool l0c = false;
            for (const auto& access : *accesses) {
                EXPECT_EQ(access.unitFlag, mode) << id;
                l0c |= access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0C;
            }
            EXPECT_TRUE(l0c) << id;
        }
    }
}
TEST(RacecheckTranslation, RawCopyFlagLoadUsesExistingCallbacks)
{
    for (bool synchronized : {false, true}) {
        npucheck::Racechecker checker;
        npucheck::CheckerReportList reports;
        AclsanLaunchData launch{};
        launch.launchId = 991;
        launch.common.version = ACLSAN_API_VERSION;
        launch.common.size = sizeof(launch);
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
        MemoryRegisterState state;
        state.dmaLoopSizes[static_cast<size_t>(DmaLoopDirection::GM_TO_CBUF)] =
            DmaLoopSizeParamField{DmaLoopDirection::GM_TO_CBUF, 1, 1};
        const auto emit = [&](uint64_t seq, uint32_t instruction, uint16_t pipe, std::array<uint64_t, 4> args) {
            ParsedTraceRecord raw{};
            raw.launchId = 991;
            raw.instrExecId = seq;
            raw.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
            raw.record.instrId = instruction;
            raw.record.pipeline = pipe;
            std::copy(args.begin(), args.end(), raw.record.args);
            auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw.record);
            ASSERT_TRUE(decoded.has_value());
            auto translated = TranslateDecodedTraceToCallbackData(raw, *decoded, state);
            ASSERT_TRUE(translated.has_value());
            if (auto* accesses = std::get_if<DeviceMemoryAccessDataList>(&*translated)) {
                ASSERT_FALSE(accesses->empty());
                for (const auto& a : *accesses)
                    ASSERT_TRUE(checker.OnCallback(
                        ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &a, reports));
            } else {
                const auto& sync = std::get<AclsanDeviceSyncData>(*translated);
                ASSERT_TRUE(
                    checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &sync, reports));
            }
        };
        emit(1, 73, ACLSAN_DEVICE_PIPE_MTE2, {0x100, 0x100000, (1ULL << 4) | (1ULL << 25), 0});
        if (synchronized) {
            emit(2, 440, ACLSAN_DEVICE_PIPE_SCALAR, {4, 3, 0, 0});
            emit(3, 442, ACLSAN_DEVICE_PIPE_SCALAR, {4, 3, 0, 0});
        }
        emit(4, 141, ACLSAN_DEVICE_PIPE_MTE1, {0, 0x100, (1ULL << 32) | (1ULL << 40), 1});
        AclsanSynchronizeData end{};
        end.common.version = ACLSAN_API_VERSION;
        end.common.size = sizeof(end);
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &end, reports));
        checker.Finalize(reports);
        EXPECT_TRUE(checker.AnalysisComplete()) << checker.Summary();
        EXPECT_EQ(reports.size(), synchronized ? 0U : 1U);
        if (!synchronized && !reports.empty()) {
            const auto& r = std::get<npucheck::NpuCheckRacecheckReport>(reports[0]);
            EXPECT_EQ(r.hazardKind, 1U);
            EXPECT_EQ(r.overlapBytes, 32U);
        }
    }
}
TEST(RacecheckTranslation, MissingRegisterStateEstimatedCallbackIsSkipped)
{
    ParsedTraceRecord raw{};
    raw.launchId = 992;
    raw.instrExecId = 1;
    raw.record.instrId = 73;
    raw.record.pipeline = 4;
    raw.record.args[2] = (1ULL << 4) | (1ULL << 25);
    auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw.record);
    ASSERT_TRUE(decoded.has_value());
    auto translated = TranslateDecodedTraceToCallbackData(raw, *decoded);
    ASSERT_TRUE(translated.has_value());
    const auto& accesses = std::get<DeviceMemoryAccessDataList>(*translated);
    ASSERT_FALSE(accesses.empty());
    EXPECT_TRUE(accesses.front().header.flags & ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED);
    npucheck::Racechecker checker;
    npucheck::CheckerReportList reports;
    AclsanLaunchData launch{};
    launch.launchId = 992;
    launch.common.version = ACLSAN_API_VERSION;
    launch.common.size = sizeof(launch);
    ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    for (const auto& access : accesses)
        ASSERT_TRUE(checker.OnCallback(
            ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    AclsanSynchronizeData end{};
    end.common.version = ACLSAN_API_VERSION;
    end.common.size = sizeof(end);
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &end, reports));
    EXPECT_TRUE(checker.AnalysisComplete());
    EXPECT_TRUE(reports.empty());
    EXPECT_NE(checker.Summary().find("skipped_memory_events=1"), std::string::npos);
}
} // namespace
