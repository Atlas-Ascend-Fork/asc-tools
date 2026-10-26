/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

// Host tests emulate only level filtering and stdout capture. Real file logging
// is verified by device smoke tests using the product's unified_dlog backend.
#include "npu_compute_log.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int32_t CheckLogLevel(int32_t module, int32_t level)
{
    const char* configured = std::getenv("ASCEND_GLOBAL_LOG_LEVEL");
    const int threshold = configured == nullptr ? DLOG_ERROR : std::atoi(configured);
    return module == ASCTOOL_MODULE_ID && level >= threshold ? 1 : 0;
}

extern "C" void DlogRecord(int32_t, int32_t, const char* format, ...)
{
    const char* stdoutMode = std::getenv("ASCEND_SLOG_PRINT_TO_STDOUT");
    if (stdoutMode == nullptr || std::strcmp(stdoutMode, "1") != 0) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stdout, format, arguments);
    va_end(arguments);
    std::fflush(stdout);
}
