// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "acl_san/aclsan_api.h"
#include "checker/checker.h"
#include "diagnostic/report/device_call_stack.h"
#include "diagnostic/report_renderer.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace npucheck {
namespace {
std::vector<std::unique_ptr<Checker>> Both()
{
    std::vector<std::unique_ptr<Checker>> result;
    result.push_back(CreateChecker(npucheck::ipc::ToolId::MEMCHECK));
    result.push_back(CreateChecker(npucheck::ipc::ToolId::SYNCCHECK));
    return result;
}

bool Dispatch(
    const std::vector<std::unique_ptr<Checker>>& checkers, AclsanCallbackDomain domain, AclsanCallbackId cbid,
    const void* data, CheckerReportList& reports)
{
    bool valid = true;
    for (const auto& checker : checkers) {
        if (checker->IsSubscribed(domain, cbid))
            valid = checker->OnCallback(domain, cbid, data, reports) && valid;
    }
    return valid;
}

AclsanDeviceSyncData Wait()
{
    AclsanDeviceSyncData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
    data.action = ACLSAN_DEVICE_SYNC_ACTION_WAIT;
    data.scope = ACLSAN_DEVICE_SYNC_SCOPE_BLOCK;
    data.srcPipe = ACLSAN_DEVICE_PIPE_VECTOR;
    data.dstPipe = ACLSAN_DEVICE_PIPE_MTE2;
    data.objectId = 7;
    return data;
}

AclsanDeviceMemoryAccessData Access()
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.address = 0x1018;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    data.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    data.accessCount = 1;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = 16;
    return data;
}

AclsanSynchronizeData Sync(int result = 0)
{
    AclsanSynchronizeData data{};
    data.common.version = ACLSAN_API_VERSION;
    data.common.size = sizeof(data);
    data.common.result = result;
    return data;
}

TEST(CheckerTest, CallbackUnionAndIndependentTools)
{
    auto checkers = Both();
    const auto callbacks = RequiredCallbacks(checkers);
    EXPECT_EQ(callbacks.size(), 6U);
    EXPECT_EQ(
        std::count_if(
            callbacks.begin(), callbacks.end(),
            [](const auto& cb) {
                const auto& [domain, cbid] = cb;
                return domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE;
            }),
        1);
    EXPECT_TRUE(checkers[0]->IsSubscribed(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL));
    EXPECT_TRUE(checkers[1]->IsSubscribed(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL));
    EXPECT_EQ(CreateChecker(static_cast<npucheck::ipc::ToolId>(0xffff)), nullptr);
}

TEST(CheckerTest, RacecheckFactoryUsesSynchronizationGraphAndWaitsForCompleteLaunch)
{
    auto checker = CreateChecker(npucheck::ipc::ToolId::RACECHECK);
    ASSERT_NE(checker, nullptr);
    EXPECT_TRUE(checker->IsSubscribed(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS));
    EXPECT_TRUE(checker->IsSubscribed(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC));
    EXPECT_FALSE(checker->AnalysisComplete());
    EXPECT_NE(checker->Summary().find("tool=racecheck"), std::string::npos);
    EXPECT_TRUE(checker->IsSubscribed(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER));
}

TEST(CheckerTest, BothProduceReportsFromTheSameSynchronization)
{
    auto checkers = Both();
    CheckerReportList reports;
    AclsanResourceData allocation{};
    allocation.common.version = ACLSAN_API_VERSION;
    allocation.common.size = sizeof(allocation);
    allocation.ptr = reinterpret_cast<void*>(0x1000);
    allocation.bytes = 32;
    allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    allocation.resourceId = 1;
    ASSERT_TRUE(Dispatch(checkers, ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &allocation, reports));
    const auto access = Access();
    const auto wait = Wait();
    ASSERT_TRUE(
        Dispatch(checkers, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    ASSERT_TRUE(Dispatch(checkers, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &wait, reports));
    EXPECT_FALSE(checkers[0]->AnalysisComplete());
    const auto sync = Sync();
    ASSERT_TRUE(
        Dispatch(checkers, ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<npucheck::NpuCheckMemcheckReport>(reports[0]));
    EXPECT_TRUE(std::holds_alternative<npucheck::NpuCheckSynccheckReport>(reports[1]));
    std::vector<npucheck::NpuCheckReportRecord> records;
    for (auto& report : reports) {
        std::visit([&](auto& typed) { records.push_back(npucheck::NpuCheckReportRecord::From(typed)); }, report);
    }
    std::string text;
    ASSERT_EQ(npucheck::RenderNpuCheckReportBundle(records, {}, &text), npucheck::ReportRenderStatus::SUCCESS);
    EXPECT_NE(text.find("MEMCHECK SUMMARY: 1 errors"), std::string::npos);
    EXPECT_NE(text.find("SYNCCHECK SUMMARY: 1 errors"), std::string::npos);
    for (const auto& checker : checkers) {
        EXPECT_TRUE(checker->HasErrors());
        EXPECT_TRUE(checker->AnalysisComplete());
        EXPECT_NE(checker->Summary().find("synchronizations=1"), std::string::npos);
    }
}

TEST(CheckerTest, FailedSynchronizationAnalyzesPendingRecords)
{
    auto checkers = Both();
    CheckerReportList reports;
    const auto access = Access();
    const auto wait = Wait();
    Dispatch(checkers, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports);
    Dispatch(checkers, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &wait, reports);
    const auto sync = Sync(1);
    EXPECT_TRUE(
        Dispatch(checkers, ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    EXPECT_FALSE(checkers[0]->AnalysisComplete());
    EXPECT_TRUE(checkers[1]->AnalysisComplete());
    EXPECT_TRUE(checkers[1]->HasErrors());
    EXPECT_TRUE(checkers[0]->HasErrors());
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<npucheck::NpuCheckMemcheckReport>(reports[0]));
    EXPECT_TRUE(std::holds_alternative<npucheck::NpuCheckSynccheckReport>(reports[1]));
    reports.clear();
    const auto nextSync = Sync();
    EXPECT_TRUE(
        Dispatch(checkers, ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &nextSync, reports));
    EXPECT_TRUE(reports.empty());
    EXPECT_NE(checkers[0]->Summary().find("pending_device_operations=0"), std::string::npos);
}

TEST(CheckerTest, CallbackMetadataIsNotRevalidated)
{
    auto checkers = Both();
    CheckerReportList reports;
    auto wait = Wait();
    wait.header.size = 1;
    EXPECT_TRUE(Dispatch(checkers, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &wait, reports));
    auto sync = Sync();
    sync.common.version = ACLSAN_API_VERSION + 1;
    EXPECT_TRUE(
        Dispatch(checkers, ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<npucheck::NpuCheckSynccheckReport>(reports.front()));
    for (const auto& checker : checkers) {
        EXPECT_TRUE(checker->AnalysisComplete());
        EXPECT_NE(checker->Summary().find("synchronizations=1"), std::string::npos);
    }
}

TEST(CallStackReportTest, MissingFramesOnlyShowsLineInformationHint)
{
    auto stack = std::make_unique<AclsanDeviceCallStack>();
    stack->pc = 0x1234;
    stack->binaryId = 42;
    stack->flags = ACLSAN_CALL_STACK_FLAG_TRUNCATED;
    EXPECT_EQ(npucheck::FormatCallStackReport(ACLSAN_STATUS_SUCCESS, *stack), "Line information unavailable.\n");
    stack->depth = 1;
    EXPECT_EQ(
        npucheck::FormatCallStackReport(ACLSAN_STATUS_ERROR_INVALID_STATE, *stack), "Line information unavailable.\n");
    npucheck::NpuCheckMemcheckReport report;
    npucheck::PopulateDeviceCallStack(report);
    ASSERT_EQ(report.common.stackCount, 1U);
    EXPECT_EQ(report.common.stacks[0].rawText, "Line information unavailable.\n");
}

TEST(CallStackReportTest, FramesContainOnlyFunctionAndSourceLocation)
{
    auto stack = std::make_unique<AclsanDeviceCallStack>();
    stack->pc = 0x1234;
    stack->binaryId = 42;
    stack->flags = ACLSAN_CALL_STACK_FLAG_TRUNCATED;
    stack->depth = 1;
    std::strcpy(stack->frames[0].functionName, "Kernel");
    std::strcpy(stack->frames[0].fileName, "kernel.asc");
    stack->frames[0].line = 12;
    stack->frames[0].column = 7;
    EXPECT_EQ(
        npucheck::FormatCallStackReport(ACLSAN_STATUS_ERROR_MAX_LIMIT_REACHED, *stack),
        "  #0 Kernel at kernel.asc:12:7\n");
    stack->frames[0].line = 0;
    EXPECT_EQ(npucheck::FormatCallStackReport(ACLSAN_STATUS_SUCCESS, *stack), "  #0 Kernel at kernel.asc\n");
    stack->depth = ACLSAN_CALL_STACK_MAX_DEPTH + 10;
    EXPECT_NO_THROW(npucheck::FormatCallStackReport(ACLSAN_STATUS_SUCCESS, *stack));
}
} // namespace
} // namespace npucheck
