/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_DIAGNOSTIC_REPORT_REPORT_CATALOG_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_DIAGNOSTIC_REPORT_REPORT_CATALOG_H

#include "diagnostic/report_renderer.h"

#include <map>

namespace npucheck::detail {

struct PatternDescriptor {
    NpuCheckReportPattern value;
    ReportTemplate reportTemplate;
};

using PatternCatalog = std::map<ReportTemplateKey, PatternDescriptor>;

const PatternCatalog& GetPatternCatalog();
const PatternDescriptor* FindPatternDescriptor(ReportTool tool, NpuCheckReportPattern pattern);
const PatternDescriptor* FindPatternDescriptor(const ReportTemplateKey& key);
const char* PatternName(ReportTool tool, NpuCheckReportPattern pattern);

const char* ToolName(ReportTool tool);
bool ParseToolName(const std::string& text, ReportTool* tool);

} // namespace npucheck::detail

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_DIAGNOSTIC_REPORT_REPORT_CATALOG_H
