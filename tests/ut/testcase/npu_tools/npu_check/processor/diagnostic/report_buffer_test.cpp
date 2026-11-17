// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "diagnostic/report_buffer.h"

#include "wire_protocol.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace npucheck::diagnostic {
namespace {

TEST(ReportBufferTest, AppendsInOrderAndTakeClears)
{
    ReportBuffer buffer;
    EXPECT_TRUE(buffer.Append("first\n"));
    EXPECT_TRUE(buffer.Append("second\n"));
    EXPECT_FALSE(buffer.Truncated());
    EXPECT_FALSE(buffer.Failed());
    EXPECT_EQ(buffer.Size(), 13U);

    EXPECT_EQ(buffer.Take(), "first\nsecond\n");
    // Take 之后缓冲区必须是空的，否则退出路径上重复取用会把报告发两遍。
    EXPECT_EQ(buffer.Size(), 0U);
    EXPECT_EQ(buffer.Take(), "");
}

TEST(ReportBufferTest, AppendingEmptyTextIsANoOp)
{
    ReportBuffer buffer;
    EXPECT_TRUE(buffer.Append(""));
    EXPECT_EQ(buffer.Size(), 0U);
    EXPECT_FALSE(buffer.Truncated());
}

// 触及上限时截到边界为止并置 truncated，而不是丢弃整份报告：已经查出来的问题仍要交付。
TEST(ReportBufferTest, TruncatesAtTheSizeLimitAndKeepsWhatFits)
{
    ReportBuffer buffer;
    const std::string block(1024u * 1024u, 'x');
    size_t written = 0;
    while (buffer.Append(block)) {
        written += block.size();
        ASSERT_LE(written, npucheck::ipc::MAX_RESULT_BYTES);
    }

    EXPECT_TRUE(buffer.Truncated());
    EXPECT_FALSE(buffer.Failed());
    // 恰好停在上限上：既没有越界，也没有因为最后一段放不下就整段丢弃。
    EXPECT_EQ(buffer.Size(), npucheck::ipc::MAX_RESULT_BYTES);

    // 已经截断之后继续追加只会被拒绝，不改变已有内容。
    EXPECT_FALSE(buffer.Append("tail"));
    EXPECT_EQ(buffer.Size(), npucheck::ipc::MAX_RESULT_BYTES);
}

// 诊断在多个 callback 线程上产生，追加必须是线程安全的。
TEST(ReportBufferTest, ConcurrentAppendsKeepEveryByte)
{
    constexpr size_t THREADS = 8;
    constexpr size_t PER_THREAD = 200;
    const std::string unit = "0123456789";

    ReportBuffer buffer;
    std::vector<std::thread> workers;
    workers.reserve(THREADS);
    for (size_t index = 0; index < THREADS; ++index) {
        workers.emplace_back([&buffer, &unit] {
            for (size_t count = 0; count < PER_THREAD; ++count) {
                (void)buffer.Append(unit);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    // 并发下不保证顺序，但一个字节都不能丢。
    EXPECT_EQ(buffer.Size(), THREADS * PER_THREAD * unit.size());
    EXPECT_FALSE(buffer.Truncated());
    EXPECT_FALSE(buffer.Failed());
}

} // namespace
} // namespace npucheck::diagnostic
