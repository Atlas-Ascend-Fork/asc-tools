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

#include "../../common/tests/plog_capture.h"
#include "device_instr/arch/dav_3510/register_state_manager.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

using aclsan::DmaLoopDirection;
using aclsan::DmaLoopSizeParamField;
using aclsan::DmaLoopStrideParamField;
using aclsan::Loop3ParamField;
using aclsan::SetPaddingParamField;
using aclsan::VectorMaskParamField;
using aclsan::dav3510::Dav3510CoreKey;
using aclsan::dav3510::Dav3510RegisterStateManager;

constexpr Dav3510CoreKey VECTOR_BLOCK_3{ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 3};
constexpr Dav3510CoreKey VECTOR_BLOCK_4{ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 4};
constexpr Dav3510CoreKey CUBE_BLOCK_3{ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 3};

constexpr size_t DirectionIndex(DmaLoopDirection direction) noexcept { return static_cast<size_t>(direction); }

template <typename Action>
std::string CaptureDebugLogs(Action action)
{
    PlogCapture capture;
    action();
    return capture.Text();
}

size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

void TestStoresDecodedRegisterState()
{
    Dav3510RegisterStateManager manager(17);
    manager.Update(VECTOR_BLOCK_3, VectorMaskParamField{0x1234, 0x5678});
    manager.Update(VECTOR_BLOCK_3, Loop3ParamField{2, 3, 4});

    const auto state = manager.Get(VECTOR_BLOCK_3);
    assert(state.has_value());
    assert(state->vectorMask.has_value());
    assert(state->vectorMask->vectorMask0 == 0x1234);
    assert(state->vectorMask->vectorMask1 == 0x5678);
    assert(state->loop3.has_value());
    assert(state->loop3->loopCount == 2);
    assert(state->loop3->srcStride == 3);
    assert(state->loop3->dstStride == 4);
}

void TestIsolatesStateByBlockTypeAndBlockId()
{
    Dav3510RegisterStateManager manager(18);
    manager.Update(VECTOR_BLOCK_3, VectorMaskParamField{1, 2});
    manager.Update(VECTOR_BLOCK_4, VectorMaskParamField{3, 4});
    manager.Update(CUBE_BLOCK_3, VectorMaskParamField{5, 6});
    manager.Update(VECTOR_BLOCK_3, Loop3ParamField{7, 8, 9});
    manager.Update(VECTOR_BLOCK_4, Loop3ParamField{10, 11, 12});
    manager.Update(CUBE_BLOCK_3, Loop3ParamField{13, 14, 15});

    assert(manager.Get(VECTOR_BLOCK_3)->vectorMask->vectorMask0 == 1);
    assert(manager.Get(VECTOR_BLOCK_4)->vectorMask->vectorMask0 == 3);
    assert(manager.Get(CUBE_BLOCK_3)->vectorMask->vectorMask0 == 5);
    assert(manager.Get(VECTOR_BLOCK_3)->loop3->loopCount == 7);
    assert(manager.Get(VECTOR_BLOCK_4)->loop3->loopCount == 10);
    assert(manager.Get(CUBE_BLOCK_3)->loop3->loopCount == 13);
}

void TestKeepsOnlyLatestValueForEachRegister()
{
    Dav3510RegisterStateManager manager(19);
    manager.Update(VECTOR_BLOCK_3, VectorMaskParamField{1, 2});
    manager.Update(VECTOR_BLOCK_3, Loop3ParamField{2, 3, 4});
    manager.Update(VECTOR_BLOCK_3, VectorMaskParamField{7, 8});

    auto state = manager.Get(VECTOR_BLOCK_3);
    assert(state->vectorMask->vectorMask0 == 7);
    assert(state->vectorMask->vectorMask1 == 8);
    assert(state->loop3->loopCount == 2);
    assert(state->loop3->srcStride == 3);
    assert(state->loop3->dstStride == 4);

    manager.Update(VECTOR_BLOCK_3, Loop3ParamField{9, 10, 11});
    state = manager.Get(VECTOR_BLOCK_3);
    assert(state->vectorMask->vectorMask0 == 7);
    assert(state->loop3->loopCount == 9);
    assert(state->loop3->srcStride == 10);
    assert(state->loop3->dstStride == 11);
}

void TestStoresLatestSetPaddingValueByCoreKey()
{
    Dav3510RegisterStateManager manager(21);
    manager.Update(VECTOR_BLOCK_3, SetPaddingParamField{UINT64_C(0xfedcba9876543210)});
    manager.Update(VECTOR_BLOCK_4, SetPaddingParamField{UINT64_C(0x1111222233334444)});
    manager.Update(CUBE_BLOCK_3, SetPaddingParamField{UINT64_C(0x5555666677778888)});
    manager.Update(VECTOR_BLOCK_3, SetPaddingParamField{UINT64_C(0x0123456789abcdef)});

    assert(manager.Get(VECTOR_BLOCK_3)->setPadding->value == UINT64_C(0x0123456789abcdef));
    assert(manager.Get(VECTOR_BLOCK_4)->setPadding->value == UINT64_C(0x1111222233334444));
    assert(manager.Get(CUBE_BLOCK_3)->setPadding->value == UINT64_C(0x5555666677778888));
}

void TestStoresLatestNdDmaPadCountByCoreKey()
{
    Dav3510RegisterStateManager manager(22);
    aclsan::NdDmaPadCountParamField first{};
    first.leftPaddingCounts = {1, 2, 3, 4};
    first.rightPaddingCounts = {5, 6, 7, 8};
    aclsan::NdDmaPadCountParamField second{};
    second.leftPaddingCounts = {11, 12, 13, 14};
    second.rightPaddingCounts = {15, 16, 17, 18};
    aclsan::NdDmaPadCountParamField latest{};
    latest.leftPaddingCounts = {21, 22, 23, 24};
    latest.rightPaddingCounts = {25, 26, 27, 28};

    manager.Update(VECTOR_BLOCK_3, first);
    manager.Update(VECTOR_BLOCK_4, second);
    manager.Update(VECTOR_BLOCK_3, latest);

    assert(manager.Get(VECTOR_BLOCK_3)->ndDmaPadCount->leftPaddingCounts == latest.leftPaddingCounts);
    assert(manager.Get(VECTOR_BLOCK_3)->ndDmaPadCount->rightPaddingCounts == latest.rightPaddingCounts);
    assert(manager.Get(VECTOR_BLOCK_4)->ndDmaPadCount->leftPaddingCounts == second.leftPaddingCounts);
    assert(manager.Get(VECTOR_BLOCK_4)->ndDmaPadCount->rightPaddingCounts == second.rightPaddingCounts);
}

void TestStoresLatestMte2SourceStrideByCoreKey()
{
    Dav3510RegisterStateManager manager(25);
    manager.Update(CUBE_BLOCK_3, aclsan::Mte2SourceParamField{-4});
    manager.Update(VECTOR_BLOCK_3, aclsan::Mte2SourceParamField{7});
    manager.Update(CUBE_BLOCK_3, aclsan::Mte2SourceParamField{-8});

    assert(manager.Get(CUBE_BLOCK_3)->mte2Source->srcStride == -8);
    assert(manager.Get(VECTOR_BLOCK_3)->mte2Source->srcStride == 7);
    assert(!manager.Get(VECTOR_BLOCK_4).has_value());
}

void TestStoresLatestDmaLoopSizeByDirectionAndCoreKey()
{
    Dav3510RegisterStateManager manager(23);
    constexpr std::array<DmaLoopDirection, 3> directions{
        DmaLoopDirection::UBUF_TO_GM, DmaLoopDirection::GM_TO_UBUF, DmaLoopDirection::GM_TO_CBUF};

    for (size_t index = 0; index < directions.size(); ++index) {
        manager.Update(
            VECTOR_BLOCK_3,
            DmaLoopSizeParamField{
                directions[index], static_cast<uint32_t>(10 + index), static_cast<uint32_t>(20 + index)});
        manager.Update(
            VECTOR_BLOCK_4,
            DmaLoopSizeParamField{
                directions[index], static_cast<uint32_t>(30 + index), static_cast<uint32_t>(40 + index)});
    }
    manager.Update(VECTOR_BLOCK_3, DmaLoopSizeParamField{DmaLoopDirection::GM_TO_UBUF, 51, 52});

    const auto block3State = manager.Get(VECTOR_BLOCK_3);
    const auto block4State = manager.Get(VECTOR_BLOCK_4);
    assert(block3State.has_value());
    assert(block4State.has_value());
    for (size_t index = 0; index < directions.size(); ++index) {
        const auto directionIndex = DirectionIndex(directions[index]);
        assert(block3State->dmaLoopSizes[directionIndex].has_value());
        assert(block4State->dmaLoopSizes[directionIndex].has_value());
        assert(block4State->dmaLoopSizes[directionIndex]->loop1Size == 30 + index);
        assert(block4State->dmaLoopSizes[directionIndex]->loop2Size == 40 + index);
    }
    const auto gmToUbufIndex = DirectionIndex(DmaLoopDirection::GM_TO_UBUF);
    assert(block3State->dmaLoopSizes[gmToUbufIndex]->loop1Size == 51);
    assert(block3State->dmaLoopSizes[gmToUbufIndex]->loop2Size == 52);
    assert(block4State->dmaLoopSizes[gmToUbufIndex]->loop1Size == 31);
    assert(block4State->dmaLoopSizes[gmToUbufIndex]->loop2Size == 41);
}

void TestStoresLatestDmaLoopStrideByDirectionLoopAndCoreKey()
{
    Dav3510RegisterStateManager manager(24);
    constexpr std::array<DmaLoopDirection, 3> directions{
        DmaLoopDirection::UBUF_TO_GM, DmaLoopDirection::GM_TO_UBUF, DmaLoopDirection::GM_TO_CBUF};

    for (size_t directionIndex = 0; directionIndex < directions.size(); ++directionIndex) {
        for (uint32_t loopIndex = 0; loopIndex < 2; ++loopIndex) {
            const uint64_t value = 100 + directionIndex * 10 + loopIndex;
            manager.Update(
                VECTOR_BLOCK_3, DmaLoopStrideParamField{directions[directionIndex], loopIndex, value, value + 100});
            manager.Update(
                VECTOR_BLOCK_4,
                DmaLoopStrideParamField{directions[directionIndex], loopIndex, value + 200, value + 300});
        }
    }
    manager.Update(
        VECTOR_BLOCK_3, DmaLoopStrideParamField{DmaLoopDirection::GM_TO_CBUF, 1, UINT64_C(0x1234), UINT64_C(0x5678)});

    const auto block3State = manager.Get(VECTOR_BLOCK_3);
    const auto block4State = manager.Get(VECTOR_BLOCK_4);
    assert(block3State.has_value());
    assert(block4State.has_value());
    for (size_t directionIndex = 0; directionIndex < directions.size(); ++directionIndex) {
        const auto stateIndex = DirectionIndex(directions[directionIndex]);
        for (uint32_t loopIndex = 0; loopIndex < 2; ++loopIndex) {
            assert(block3State->dmaLoopStrides[stateIndex][loopIndex].has_value());
            assert(block4State->dmaLoopStrides[stateIndex][loopIndex].has_value());
        }
    }
    const auto gmToCbufIndex = DirectionIndex(DmaLoopDirection::GM_TO_CBUF);
    assert(block3State->dmaLoopStrides[gmToCbufIndex][1]->srcStride == UINT64_C(0x1234));
    assert(block3State->dmaLoopStrides[gmToCbufIndex][1]->dstStride == UINT64_C(0x5678));
    assert(block4State->dmaLoopStrides[gmToCbufIndex][1]->srcStride == 321);
    assert(block4State->dmaLoopStrides[gmToCbufIndex][1]->dstStride == 421);
}

void TestResetClearsStateAndKeepsLaunchIdentity()
{
    Dav3510RegisterStateManager manager(20);
    manager.Update(VECTOR_BLOCK_3, VectorMaskParamField{1, 2});

    manager.Reset();

    assert(manager.GetLaunchId() == 20);
    assert(!manager.Get(VECTOR_BLOCK_3).has_value());
}

void TestLogsEveryRegisterUpdateWithFullNewValue()
{
    Dav3510RegisterStateManager manager(17);
    const std::string logs = CaptureDebugLogs([&manager] {
        manager.Update(VECTOR_BLOCK_3, VectorMaskParamField{0x1234, 0x5678});
        manager.Update(VECTOR_BLOCK_3, VectorMaskParamField{0x9abc, 0xdef0});
        manager.Update(VECTOR_BLOCK_3, Loop3ParamField{2, 3, 4});
        manager.Update(VECTOR_BLOCK_3, DmaLoopSizeParamField{DmaLoopDirection::GM_TO_CBUF, 5, 6});
        manager.Update(VECTOR_BLOCK_3, DmaLoopStrideParamField{DmaLoopDirection::GM_TO_CBUF, 1, 7, 8});
        manager.Update(VECTOR_BLOCK_3, SetPaddingParamField{UINT64_C(0xfedcba9876543210)});
    });

    assert(CountOccurrences(logs, "action=update register=vector_mask") == 2);
    assert(
        logs.find("[register] action=update register=vector_mask launchId=17 blockType=1 blockId=3 "
                  "vectorMask0=0x1234 vectorMask1=0x5678") != std::string::npos);
    assert(
        logs.find("[register] action=update register=vector_mask launchId=17 blockType=1 blockId=3 "
                  "vectorMask0=0x9abc vectorMask1=0xdef0") != std::string::npos);
    assert(
        logs.find("[register] action=update register=loop3 launchId=17 blockType=1 blockId=3 "
                  "loopCount=2 srcStride=3 dstStride=4") != std::string::npos);
    assert(
        logs.find("[register] action=update register=dma_loop_size launchId=17 blockType=1 blockId=3 "
                  "direction=2 loop1Size=5 loop2Size=6") != std::string::npos);
    assert(
        logs.find("[register] action=update register=dma_loop_stride launchId=17 blockType=1 blockId=3 "
                  "direction=2 loopIndex=1 srcStride=7 dstStride=8") != std::string::npos);
    assert(
        logs.find("[register] action=update register=set_padding launchId=17 blockType=1 blockId=3 "
                  "value=0xfedcba9876543210") != std::string::npos);
    assert(logs.find("instrExecId") == std::string::npos);
    assert(logs.find("config0") == std::string::npos);
}

} // namespace

TEST(RegisterStateManager, Main)
{
    TestStoresDecodedRegisterState();
    TestIsolatesStateByBlockTypeAndBlockId();
    TestKeepsOnlyLatestValueForEachRegister();
    TestStoresLatestSetPaddingValueByCoreKey();
    TestStoresLatestNdDmaPadCountByCoreKey();
    TestStoresLatestMte2SourceStrideByCoreKey();
    TestStoresLatestDmaLoopSizeByDirectionAndCoreKey();
    TestStoresLatestDmaLoopStrideByDirectionLoopAndCoreKey();
    TestResetClearsStateAndKeepsLaunchIdentity();
    TestLogsEveryRegisterUpdateWithFullNewValue();
}

TEST(RegisterStateManager, AtomicCtrlIsolatesExecutionCoresAndResetsBetweenLaunches)
{
    Dav3510RegisterStateManager manager(1);
    const Dav3510CoreKey first{ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 0, 1, 0};
    const Dav3510CoreKey second{ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 0, 2, 0};
    const Dav3510CoreKey otherDevice{ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 0, 1, 1};
    manager.Update(first, aclsan::CtrlRegisterParamField{64});
    manager.Update(second, aclsan::CtrlRegisterParamField{0});
    EXPECT_EQ(manager.Get(first)->ctrl->value, 64U);
    EXPECT_EQ(manager.Get(second)->ctrl->value, 0U);
    EXPECT_FALSE(manager.Get(otherDevice).has_value());
    manager.Update(first, aclsan::CtrlRegisterParamField{128});
    EXPECT_EQ(manager.Get(first)->ctrl->value, 128U);
    EXPECT_FALSE(Dav3510RegisterStateManager(2).Get(first).has_value());
    manager.Reset();
    EXPECT_FALSE(manager.Get(first).has_value());
}
