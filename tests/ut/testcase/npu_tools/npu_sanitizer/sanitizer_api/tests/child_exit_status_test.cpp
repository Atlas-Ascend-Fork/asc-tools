/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "dbi/tool_runner.h"

#include <cstdio>
#include <sys/wait.h>

int main()
{
    // Cover every Linux wait status encoding, including stopped and continued children.
    constexpr int MAX_WAIT_STATUS = 0xffff;
    for (int status = 0; status <= MAX_WAIT_STATUS; ++status) {
        const auto decoded = aclsan::DecodeChildExitStatus(status);
        if (decoded.exited != (WIFEXITED(status) != 0) || decoded.signaled != (WIFSIGNALED(status) != 0) ||
            (decoded.exited && decoded.exitCode != WEXITSTATUS(status)) ||
            (decoded.signaled && decoded.signalNumber != WTERMSIG(status))) {
            std::fprintf(stderr, "Child exit status differs from system macros: status=%d\n", status);
            return 1;
        }
    }
    std::puts("All 65536 Linux wait status encodings match the system macros.");
    return 0;
}
