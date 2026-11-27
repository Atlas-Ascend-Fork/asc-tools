/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>
#define private public
#define protected public
#include "stub_def.h"
#include "stub_fun.h"
#include "mockcpp/mockcpp.hpp"
#include "api_check/kernel_cpu_check.h"

using namespace std;

class TestCpuDebugApiCheckSuite : public testing::Test {
protected:
    void SetUp() {}
    void TearDown()
    {
        block_idx = 0;
        block_num = 8;
        g_coreType = 0;
        g_taskRation = 2;
        sub_block_idx = 0;
        GlobalMockObject::verify();
    }
};

TEST_F(TestCpuDebugApiCheckSuite, CpuDebugApiCheckSharedMem)
{
    uint8_t* x;
    x = (uint8_t*)AscendC::GmAlloc(8);
    EXPECT_TRUE(x != nullptr);
    uint8_t* y = (uint8_t*)AscendC::get_workspace(4);
    uint64_t kargs[1];
    kargs[0] = (uint64_t)x;
    AscendC::CheckGmValied(1, kargs);
    AscendC::GmFree((void*)x);
    g_workspaceSharedPtr = 0;
    g_fullSizeOfWorkspace = 0;
}

TEST_F(TestCpuDebugApiCheckSuite, CpuDebugApiCheckSharedMemDeathTest)
{
    EXPECT_DEATH(
        { AscendC::GmAlloc(SIZE_MAX); },
        "GmAlloc Error: input size [0-9]+ exceeds maximum allowed size [0-9]+ bytes\\.");
    EXPECT_DEATH(
        { AscendC::GmAlloc(184467440736432); }, "GmAlloc Error: The /tmp directory does not have enough space.");
}

TEST_F(TestCpuDebugApiCheckSuite, CpuDebugApiCheckBlock)
{
    int idx = 4;
    AscendC::set_core_type(idx);
    AscendC::set_block_dim(idx);
    int blocknum = AscendC::get_block_num();
    int blockidx = AscendC::get_block_idx();
    int processnum = AscendC::get_process_num();
    EXPECT_EQ(sub_block_idx, 0);
    EXPECT_EQ(g_taskRation, 2);
    EXPECT_EQ(blocknum, 8);
    EXPECT_EQ(blockidx, 4);
    EXPECT_EQ(processnum, 8);
}

TEST_F(TestCpuDebugApiCheckSuite, GetHardWarebufferSize)
{
    uint64_t L0ASize = AscendC::check::GetHardWarebufferSize(3);
    EXPECT_EQ(L0ASize, 65536);
}
