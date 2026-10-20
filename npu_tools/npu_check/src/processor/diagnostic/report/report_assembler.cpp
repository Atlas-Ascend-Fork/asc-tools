/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "diagnostic/report/report_assembler.h"
#include "diagnostic/report/device_call_stack.h"
#include "diagnostic/report/report_fields.h"
#include "diagnostic/report/report_normalizer.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace npucheck {
ReportAssemblyResult AssembleReport(CheckerReport& report)
{
    return std::visit(
        [](auto& typed) {
            ReportAssemblyResult result;
            if constexpr (std::is_same_v<std::decay_t<decltype(typed)>, NpuCheckRacecheckReport>) {
                result.reportId = typed.reportId;
            } else {
                PopulateDeviceCallStack(typed);
                result.reportId = typed.common.reportId;
            }
            result.status = detail::NormalizeReport(NpuCheckReportRecord::From(typed), &result.record);
            if (result.status != ReportRenderStatus::SUCCESS) {
                return result;
            }
            if constexpr (std::is_same_v<std::decay_t<decltype(typed)>, NpuCheckRacecheckReport>) {
                // 在最终组装时解析双方 PC，原始证据不保存解析后的调用栈。
                for (const auto& [pc, role] :
                     {std::make_pair(typed.current.header.pc, ReportStackRole::RELATED_ACCESS_B),
                      std::make_pair(typed.previous.header.pc, ReportStackRole::RELATED_ACCESS_A)}) {
                    auto resolved = ResolveDeviceCallStack(pc);
                    try {
                        if (role == ReportStackRole::RELATED_ACCESS_B && resolved.binaryId != 0) {
                            result.record.fields["binaryId"] = std::to_string(resolved.binaryId);
                        }
                        if (resolved.frames.empty()) {
                            continue; // 保留归一化时生成的 PC 和调用栈不可用提示。
                        }
                        const auto stack = std::find_if(
                            result.record.stacks.begin(), result.record.stacks.end(),
                            [stackRole = role](const auto& candidate) { return candidate.role == stackRole; });
                        if (stack == result.record.stacks.end()) {
                            continue;
                        }
                        detail::PutFrameLocationFields(
                            resolved.frames.front(), role == ReportStackRole::RELATED_ACCESS_A ? "first" : "second",
                            &result.record.fields);
                        stack->format = ReportStackFormat::FRAMES;
                        stack->rawText.clear();
                        stack->frames = std::move(resolved.frames);
                    } catch (...) {
                        // 可选的调用栈补充失败时仍保留已经确认的报告。
                    }
                }
            }
            return result;
        },
        report);
}
} // namespace npucheck
