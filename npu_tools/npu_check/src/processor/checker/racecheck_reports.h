/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_RACECHECK_REPORTS_H
#define NPU_TOOLS_NPU_CHECK_RACECHECK_REPORTS_H
#include "checker/mem_state_tracker.h"
#include "diagnostic/report_message.h"
#include "diagnostic/report_renderer.h"

namespace npucheck {
// 按输入顺序报告每一条已确认冲突，不筛选或合并证据。
// 在提交内存状态前复制双方 cbdata，并解析各自的连续访问段。
// 返回值不引用 records；调用栈留到最终组装时解析。
// 每条记录的布局在本批准备阶段只展开一次，更新成功后才能发布这些报告。
std::vector<NpuCheckRacecheckReport> MakeRacecheckDiagnostics(
    const std::vector<AccessPair>& evidence, const std::vector<AclsanDeviceMemoryAccessData>& records);
// current 必须由同一条不可变 records[currentIndex] 解析；本批 evidence 均针对该当前访问。
// 直接引用当前范围，历史访问仍使用批内缓存；返回的报告不引用 current。
std::vector<NpuCheckRacecheckReport> MakeRacecheckDiagnostics(
    const std::vector<AccessPair>& evidence, const std::vector<AclsanDeviceMemoryAccessData>& records,
    CbIndex currentIndex, const racecheck::PreparedAccess& current);
// 校验已保存的身份、模式及连续范围，生成统一记录；不重新展开布局。
ReportRenderStatus NormalizeRacecheckReport(const NpuCheckRacecheckReport& report, ReportRecord* out);
} // namespace npucheck
#endif
