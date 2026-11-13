/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "acl_pti/data/data_types.h"
#include "artifact_sink.h"
#include "plog_capture.h"
#include "pmu/pmu_metric_builder.h"
#include "report/report_writer.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {
using npucompute::PmuMetricBuilder;

struct TaskIdentity {
    uint64_t replayId;
    uint16_t taskId;
    uint16_t streamId;
};

struct CounterRange {
    uint64_t start;
    uint64_t end;
};

constexpr uint8_t SqeValue(aclptiDavidSqeType type) { return static_cast<uint8_t>(type); }

constexpr uint8_t PLACEHOLDER_SQE_TYPE = SqeValue(aclptiDavidSqeType::Placeholder);

void AddTaskLogs(
    aclptiProfilingDataResult& result, TaskIdentity task, CounterRange counter,
    uint8_t sqeType = SqeValue(aclptiDavidSqeType::Aiv))
{
    auto& logs = result.taskLogs[task.taskId];
    logs.push_back(
        {task.replayId, aclpti::data::kTaskStartFunctionType, task.taskId, task.streamId, counter.start, 0, 0,
         ACLPTI_CORE_TYPE_AIV, 0, sqeType});
    logs.push_back(
        {task.replayId, aclpti::data::kTaskEndFunctionType, task.taskId, task.streamId, counter.end, 0, 0,
         ACLPTI_CORE_TYPE_AIV, 0, sqeType});
}

class SummarySink final : public npucompute::ArtifactSink {
public:
    void Begin(const std::string&) override { content.clear(); }
    void Write(std::string_view bytes) override { content.append(bytes); }
    void Commit(uint64_t) override {}
    void Abort() noexcept override {}
    std::string content;
};

TEST(PmuTaskDuration, CapturedAddReplaysExcludeControlTasksWithoutPmuOrBlockLogs)
{
    aclptiProfilingDataResult result;
    const std::array<uint64_t, 3> starts{2253017023720911ULL, 2253017168044321ULL, 2253017304186308ULL};
    const std::array<uint64_t, 3> ticks{3106, 2506, 2611};
    for (uint16_t replay = 0; replay < starts.size(); ++replay) {
        const uint16_t stream = 6 + 3 * replay;
        AddTaskLogs(
            result, {replay, 0, static_cast<uint16_t>(stream - 1)}, {starts[replay] - 1000, starts[replay] - 993},
            PLACEHOLDER_SQE_TYPE);
        AddTaskLogs(result, {replay, 0, stream}, {starts[replay], starts[replay] + ticks[replay]});
        AddTaskLogs(
            result, {replay, 0, static_cast<uint16_t>(stream + 1)}, {starts[replay] + 10000, starts[replay] + 10007},
            PLACEHOLDER_SQE_TYPE);
    }
    ASSERT_TRUE(PmuMetricBuilder::TaskDuration(result).has_value());
    EXPECT_DOUBLE_EQ(*PmuMetricBuilder::TaskDuration(result), 2.611);
    SummarySink sink;
    EXPECT_EQ(npucompute::WriteSummaryJsonl(result, {}, {}, {}, sink), ACLPTI_SUCCESS);
    EXPECT_NE(sink.content.find("\"Task Duration(us)\":2.611000"), std::string::npos);
}

TEST(PmuTaskDuration, SelectsKernelBySqeTypeInsteadOfDurationOrOrder)
{
    aclptiProfilingDataResult result;
    AddTaskLogs(result, {0, 1, 2}, {100, 10100}, SqeValue(aclptiDavidSqeType::Aiv));
    AddTaskLogs(result, {0, 1, 3}, {100, 100100}, PLACEHOLDER_SQE_TYPE);
    std::reverse(result.taskLogs[1].begin(), result.taskLogs[1].end());
    ASSERT_TRUE(PmuMetricBuilder::TaskDuration(result).has_value());
    EXPECT_DOUBLE_EQ(*PmuMetricBuilder::TaskDuration(result), 10);
}

TEST(PmuTaskDuration, MultipleValidTasksWarnAndWriteUnavailableSummary)
{
    aclptiProfilingDataResult result;
    AddTaskLogs(result, {0, 1, 2}, {100, 2100}, SqeValue(aclptiDavidSqeType::Aiv));
    AddTaskLogs(result, {0, 2, 2}, {100, 4100}, SqeValue(aclptiDavidSqeType::Sdma));
    std::string warning;
    SummarySink sink;
    npu_compute_test::CapturePlog(
        [&] { EXPECT_EQ(npucompute::WriteSummaryJsonl(result, {}, {}, {}, sink), ACLPTI_SUCCESS); }, &warning,
        DLOG_WARN);
    EXPECT_NE(warning.find("replay=0 has 2 valid non-placeholder tasks"), std::string::npos);
    EXPECT_NE(sink.content.find("\"Task Duration(us)\":\"NA\""), std::string::npos);
}

TEST(PmuTaskDuration, MalformedKernelEndpointsRemainUnavailable)
{
    for (int invalid = 0; invalid < 7; ++invalid) {
        SCOPED_TRACE(invalid);
        aclptiProfilingDataResult result;
        AddTaskLogs(result, {0, 1, 2}, {100, 2100});
        auto& logs = result.taskLogs[1];
        if (invalid < 2) {
            const auto duplicate = logs[invalid];
            logs.push_back(duplicate);
        } else if (invalid < 4) {
            logs.erase(logs.begin() + invalid - 2);
        } else if (invalid < 6) {
            logs.back().systemCounter = invalid == 4 ? 100 : 99;
        } else {
            logs.back().sqeType = SqeValue(aclptiDavidSqeType::Aic);
        }
        EXPECT_FALSE(PmuMetricBuilder::TaskDuration(result).has_value());
    }
}

TEST(PmuTaskDuration, InvalidReplayAndControlRecordsDoNotPoisonValidReplay)
{
    aclptiProfilingDataResult result;
    AddTaskLogs(result, {0, 1, 2}, {100, 2100});
    result.taskLogs[1].pop_back();
    AddTaskLogs(result, {1, 1, 2}, {100, 4100});
    result.taskLogs[2].push_back(
        {1, aclpti::data::kTaskStartFunctionType, 2, 3, 1, 0, 0, ACLPTI_CORE_TYPE_AIV, 0, PLACEHOLDER_SQE_TYPE});
    ASSERT_TRUE(PmuMetricBuilder::TaskDuration(result).has_value());
    EXPECT_DOUBLE_EQ(*PmuMetricBuilder::TaskDuration(result), 4);
}
} // namespace
