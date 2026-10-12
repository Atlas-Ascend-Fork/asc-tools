/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "options.h"
#include "process_runner.h"

#include <iostream>

int main(int argc, char** argv)
{
    constexpr int USAGE_ERROR_EXIT_CODE = 64;
    npucheck::Options options{};
    std::string error;
    // 早期失败不再向终端打机器格式的结果摘要，只落 plog。用法错误已有专属报错和用法
    // 说明，再补一句"工具执行失败"反而会把用户引去翻 plog 找不存在的故障；环境类失败
    // 属于工具内部流程失败，按 infra_failed 的打屏规则补一句人类可读提示。
    const auto reportEarlyFailure = [](int exitCode, bool withScreenNotice) {
        npucheck::ResultSummary summary;
        summary.outcome = npucheck::Outcome::INFRA_FAILED;
        summary.exit = exitCode;
        npucheck::LogResultSummary(summary);
        if (withScreenNotice) {
            std::cerr << npucheck::ScreenNotice(summary) << '\n';
        }
        return exitCode;
    };

    if (!npucheck::ParseOptions(argc, argv, options, error)) {
        std::cerr << "npu_check: " << error << "\n\n" << npucheck::Usage();
        return reportEarlyFailure(USAGE_ERROR_EXIT_CODE, false);
    }
    if (options.showHelp) {
        std::cout << npucheck::Usage();
        return 0;
    }
    // 注入库定位没有命令行入口，也没有环境变量覆盖：候选全部限定在 ASCEND_TOOLKIT_HOME
    // 这一个 CANN 根之下，顺序固定为 <arch>-linux/tools/npu_tools/lib64，其次 tools/npu_tools/lib64。
    //
    // 这里退 125 而不是 64：64 是用法错误，而定位失败属于 fork 前的准备错误 ——
    // 用户的命令行没有任何问题，是环境或安装不完整。两者必须能被脚本区分开。
    std::string libraryPath;
    if (!npucheck::ResolveLibraryPath(std::string{}, libraryPath, error)) {
        std::cerr << "npu_check: " << error << '\n';
        return reportEarlyFailure(npucheck::INFRASTRUCTURE_ERROR_EXIT_CODE, true);
    }
    return npucheck::RunApplication(options, libraryPath);
}
