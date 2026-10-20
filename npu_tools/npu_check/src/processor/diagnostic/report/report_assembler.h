/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_REPORT_ASSEMBLER_H
#define NPU_TOOLS_NPU_CHECK_REPORT_ASSEMBLER_H

#include "checker/checker.h"
#include "diagnostic/report_renderer.h"

namespace npucheck {
struct ReportAssemblyResult {
    ReportRenderStatus status = ReportRenderStatus::INVALID_ARGUMENT;
    uint64_t reportId = 0;
    ReportRecord record{};
};

// 完成归一化和可选的调用栈补充后返回最终报告。
ReportAssemblyResult AssembleReport(CheckerReport& report);
} // namespace npucheck

#endif
