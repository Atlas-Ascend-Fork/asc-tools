/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_CLI_PROCESS_RUNNER_H
#define NPU_TOOLS_NPU_CHECK_SRC_CLI_PROCESS_RUNNER_H

#include "options.h"

#include <string>

namespace npucheck {

constexpr int INFRASTRUCTURE_ERROR_EXIT_CODE = 125;

// 三类结果。分类只服务于两件事：plog 里摘要行的日志级别，以及终端上是否给出、给出
// 哪条人类可读提示（ScreenNotice）。机器格式的摘要行一律不打屏，只落 plog。
enum class Outcome {
    FORWARDED,    // 完整 Result 已转发，应用自身正常结束
    APP_FAILED,   // 完整 Result 已转发，但应用自身失败
    INFRA_FAILED, // 握手 / 协议 / Result 不完整，本次检查没有可信结论
};

struct ResultSummary {
    Outcome outcome = Outcome::INFRA_FAILED;
    int hasErrors = -1;             // <0 表示 unknown（未收到完整 Result）
    bool truncated = false;         // 报告因触及总长上限被截断
    std::string childExit = "none"; // 退出码、"signal:N"，或没有子进程时的 "none"
    int exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
};

// 机器格式的结果摘要行。只用于 plog 落盘，不属于终端输出契约。
std::string FormatResultSummary(const ResultSummary& summary);

// 把摘要行按结果级别写入 plog（forwarded=info / app_failed=warning / infra_failed=error）。
// 任何返回路径都必须落一次盘。
void LogResultSummary(const ResultSummary& summary);

// 终端上的人类可读提示：app_failed / infra_failed 各给一句明确说明，forwarded 与其余
// 没有可明确告知内容的场景返回空串，调用方拿到空串不得打屏。
std::string ScreenNotice(const ResultSummary& summary);

int RunApplication(const Options& options, const std::string& libraryPath);

} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_CLI_PROCESS_RUNNER_H
