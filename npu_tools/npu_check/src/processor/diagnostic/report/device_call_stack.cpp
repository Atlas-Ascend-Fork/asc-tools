/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "diagnostic/report/device_call_stack.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <sstream>

namespace npucheck {
namespace {
bool HasCallStackFrames(AclsanStatus status)
{
    return status == ACLSAN_STATUS_SUCCESS || status == ACLSAN_STATUS_ERROR_MAX_LIMIT_REACHED;
}

std::vector<ReportFrame> MakeReportFrames(const AclsanDeviceCallStack& stack)
{
    std::vector<ReportFrame> frames;
    const auto depth = std::min(stack.depth, static_cast<uint32_t>(ACLSAN_CALL_STACK_MAX_DEPTH));
    frames.reserve(depth);
    for (uint32_t i = 0; i < depth; ++i) {
        const auto& source = stack.frames[i];
        ReportFrame frame;
        frame.pc = stack.pc;
        frame.function.assign(source.functionName, strnlen(source.functionName, sizeof(source.functionName)));
        frame.file.assign(source.fileName, strnlen(source.fileName, sizeof(source.fileName)));
        frame.line = source.line;
        frame.column = source.column;
        frame.inlineDepth = source.inlineDepth;
        frames.push_back(std::move(frame));
    }
    return frames;
}

void PopulateStack(NpuCheckReportCommon& common, NpuCheckReportExecContext& exec, ReportStackRole role) noexcept
{
    if (common.stackCount > kNpuCheckReportStackMax) {
        return;
    }
    auto index = common.stackCount;
    for (uint32_t i = 0; i < common.stackCount; ++i) {
        if (common.stacks[i].role == role) {
            index = i;
            break;
        }
    }
    if (index == kNpuCheckReportStackMax) {
        return;
    }
    try {
        auto source = std::make_unique<AclsanDeviceCallStack>();
        const auto status =
            exec.pc == 0 ? ACLSAN_STATUS_ERROR_INVALID_STATE : aclsanGetDeviceCallStack(exec.pc, source.get());
        ReportCallStack stack;
        stack.role = role;
        if (HasCallStackFrames(status)) {
            stack.frames = MakeReportFrames(*source);
        }
        stack.format = stack.frames.empty() ? ReportStackFormat::RAW_TEXT : ReportStackFormat::FRAMES;
        if (stack.frames.empty()) {
            stack.rawText = FormatCallStackReport(status, *source);
        }
        common.stacks[index] = std::move(stack);
        if (source->binaryId != 0) {
            exec.binaryId = source->binaryId;
        }
        if (index == common.stackCount) {
            ++common.stackCount;
        }
    } catch (...) {
        // Report enrichment must not interrupt checker processing.
    }
}
} // namespace

void PopulateDeviceCallStack(NpuCheckRacecheckReport& report) noexcept
{
    PopulateStack(report.common, report.first.exec, ReportStackRole::RELATED_ACCESS_A);
    PopulateStack(report.common, report.second.exec, ReportStackRole::RELATED_ACCESS_B);
}

std::string FormatCallStackReport(AclsanStatus status, const AclsanDeviceCallStack& stack)
{
    if (!HasCallStackFrames(status) || stack.depth == 0) {
        return "Line information unavailable.\n";
    }
    std::ostringstream output;
    const auto frames = MakeReportFrames(stack);
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        output << "  #" << i << ' ' << (frame.function.empty() ? "<unknown>" : frame.function);
        if (!frame.file.empty()) {
            output << " at " << frame.file;
            if (frame.line != 0) {
                output << ':' << frame.line;
            }
            if (frame.line != 0 && frame.column != 0) {
                output << ':' << frame.column;
            }
        }
        output << '\n';
    }
    return output.str();
}

void PopulateDeviceCallStack(NpuCheckMemcheckReport& report) noexcept
{
    PopulateStack(report.common, report.common.exec, ReportStackRole::FAULT_DEVICE);
}

void PopulateDeviceCallStack(NpuCheckInitcheckReport& report) noexcept
{
    PopulateStack(report.common, report.common.exec, ReportStackRole::FAULT_DEVICE);
}

void PopulateDeviceCallStack(NpuCheckSynccheckReport& report) noexcept
{
    if (report.triggerPoint.hasExecContext) {
        PopulateStack(report.common, report.triggerPoint.exec, ReportStackRole::SYNC_TRIGGER);
        report.triggerPoint.stackRole = ReportStackRole::SYNC_TRIGGER;
        report.common.exec = report.triggerPoint.exec;
    }
    if (report.relatedPoint.hasExecContext) {
        PopulateStack(report.common, report.relatedPoint.exec, ReportStackRole::SYNC_RELATED);
        report.relatedPoint.stackRole = ReportStackRole::SYNC_RELATED;
    }
}
} // namespace npucheck
