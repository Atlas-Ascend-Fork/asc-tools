/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TOOL_RUNNER_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TOOL_RUNNER_H

#include <string>
#include <vector>

namespace aclsan {

struct ChildExitStatus {
    bool exited;
    bool signaled;
    int exitCode;
    int signalNumber;
};

inline ChildExitStatus DecodeChildExitStatus(int status) noexcept
{
    // Linux stores the exit code in bits 8..15 and the terminating signal in bits 0..6.
    constexpr unsigned int SIGNAL_MASK = 0x7fU;
    constexpr unsigned int EXIT_CODE_MASK = 0xff00U;
    constexpr unsigned int EXIT_CODE_SHIFT = 8U;
    constexpr unsigned int STOPPED_SIGNAL = 0x7fU;
    const auto bits = static_cast<unsigned int>(status);
    const unsigned int signalNumber = bits & SIGNAL_MASK;
    return {
        signalNumber == 0U, signalNumber != 0U && signalNumber != STOPPED_SIGNAL,
        static_cast<int>((bits & EXIT_CODE_MASK) >> EXIT_CODE_SHIFT), static_cast<int>(signalNumber)};
}

struct ToolResult {
    int exitCode = -1;
    std::string standardOutput;
    std::string standardError;
};

ToolResult RunTool(const std::vector<std::string>& commandArguments);

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TOOL_RUNNER_H
