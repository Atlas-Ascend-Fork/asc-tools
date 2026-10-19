/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_CHECKER_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_CHECKER_H

#include "acl_san/aclsan_api.h"
#include "diagnostic/report_message.h"
#include "wire_protocol.h"
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace npucheck {
using CallbackSpec = std::pair<AclsanCallbackDomain, AclsanCallbackId>;
using CheckerReport = std::variant<
    npucheck::NpuCheckMemcheckReport, npucheck::NpuCheckInitcheckReport, npucheck::NpuCheckSynccheckReport,
    npucheck::NpuCheckRacecheckReport>;
using CheckerReportList = std::vector<CheckerReport>;

// Checkers declare their own events and state. ToolManager serializes callbacks,
// unions subscriptions, and aggregates reports without knowing concrete checkers.
class Checker {
public:
    virtual ~Checker() = default;
    // 获取checker订阅的回调事件列表
    virtual const std::vector<CallbackSpec>& GetSubscribedID() const = 0;
    // checker自己的callback逻辑，False 表示数据格式错误。无关事件会通过 IsSubscribed() 进行过滤。
    virtual bool OnCallback(
        AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports) = 0;
    // Called after callbacks are drained; pending work must not silently disappear.
    virtual void Finalize([[maybe_unused]] CheckerReportList& reports) {}
    // 输出分析结果摘要，供工具管理器在分析结束时输出
    virtual std::string Summary() const = 0;
    // 检查是否检测到错误
    virtual bool HasErrors() const = 0;
    // 检查分析是否完成
    virtual bool AnalysisComplete() const = 0;
    // 检查是否订阅了指定的回调事件
    bool IsSubscribed(AclsanCallbackDomain domain, AclsanCallbackId cbid) const;
};

std::unique_ptr<Checker> CreateChecker(npucheck::ipc::ToolId tool);
std::vector<CallbackSpec> RequiredCallbacks(const std::vector<std::unique_ptr<Checker>>& checkers);

template <class T>
void AppendCheckerReports(std::vector<T> source, CheckerReportList& target)
{
    for (auto& report : source) {
        target.emplace_back(std::move(report));
    }
}
} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_CHECKER_H
