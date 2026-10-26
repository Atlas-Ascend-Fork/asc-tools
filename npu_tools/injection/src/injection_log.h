/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef ACL_TOOL_INJECTION_LOG_H
#define ACL_TOOL_INJECTION_LOG_H

#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>
#include "dlog_pub.h"

#define ASCTOOL_MODULE_ID static_cast<int32_t>(ASCTOOL)
#define ASCTOOL_DEBUG_ENABLED() (CheckLogLevel(ASCTOOL_MODULE_ID, DLOG_DEBUG) == 1)

// Append an empty string so C++17 callers can omit format arguments without GNU extensions.
#define ASCTOOL_DEBUG(...) ASCTOOL_DEBUG_IMPL(__VA_ARGS__, "")

#define ASCTOOL_DEBUG_IMPL(format, ...)                                                                            \
    do {                                                                                                           \
        dlog_debug(                                                                                                \
            ASCTOOL_MODULE_ID, " %ld [%s:%d][%s]" format "\n%s", static_cast<long>(syscall(SYS_gettid)), __FILE__, \
            __LINE__, __FUNCTION__, __VA_ARGS__);                                                                  \
    } while (0)

#define ASCTOOL_INFO(...) ASCTOOL_INFO_IMPL(__VA_ARGS__, "")

#define ASCTOOL_INFO_IMPL(format, ...)                                                                             \
    do {                                                                                                           \
        dlog_info(                                                                                                 \
            ASCTOOL_MODULE_ID, " %ld [%s:%d][%s]" format "\n%s", static_cast<long>(syscall(SYS_gettid)), __FILE__, \
            __LINE__, __FUNCTION__, __VA_ARGS__);                                                                  \
    } while (0)

#define ASCTOOL_WARNING(...) ASCTOOL_WARNING_IMPL(__VA_ARGS__, "")

#define ASCTOOL_WARNING_IMPL(format, ...)                                                                          \
    do {                                                                                                           \
        dlog_warn(                                                                                                 \
            ASCTOOL_MODULE_ID, " %ld [%s:%d][%s]" format "\n%s", static_cast<long>(syscall(SYS_gettid)), __FILE__, \
            __LINE__, __FUNCTION__, __VA_ARGS__);                                                                  \
    } while (0)

#define ASCTOOL_ERROR(...) ASCTOOL_ERROR_IMPL(__VA_ARGS__, "")

#define ASCTOOL_ERROR_IMPL(format, ...)                                                                            \
    do {                                                                                                           \
        dlog_error(                                                                                                \
            ASCTOOL_MODULE_ID, " %ld [%s:%d][%s]" format "\n%s", static_cast<long>(syscall(SYS_gettid)), __FILE__, \
            __LINE__, __FUNCTION__, __VA_ARGS__);                                                                  \
    } while (0)

#endif // ACL_TOOL_INJECTION_LOG_H
