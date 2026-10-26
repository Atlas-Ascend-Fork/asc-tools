/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_TEST_PLOG_CAPTURE_H
#define NPU_COMPUTE_TEST_PLOG_CAPTURE_H
#include "npu_compute_log.h"
#include "plog_test_library.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
namespace npu_compute_test {
inline std::mutex captureMutex;
inline std::string* captureOutput = nullptr;
inline int32_t captureThreshold = DLOG_DEBUG;
inline int32_t CheckCaptureLevel(int32_t module, int32_t level)
{
    std::lock_guard<std::mutex> lock(captureMutex);
    return module == ASCTOOL_MODULE_ID && level >= captureThreshold ? 1 : 0;
}
inline void CaptureRecord(int32_t, int32_t, const char* format, ...)
{
    char buffer[4096]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(captureMutex);
    if (captureOutput != nullptr)
        captureOutput->append(buffer);
}
class CaptureScope {
public:
    CaptureScope(std::string* output, int32_t threshold)
    {
        {
            std::lock_guard<std::mutex> lock(captureMutex);
            captureOutput = output;
            captureThreshold = threshold;
        }
        plog_test::SetApi({CheckCaptureLevel, CaptureRecord});
    }
    ~CaptureScope()
    {
        plog_test::ResetApi();
        std::lock_guard<std::mutex> lock(captureMutex);
        captureOutput = nullptr;
    }
};
template <typename Function>
bool CapturePlog(Function function, std::string* output, int32_t threshold = DLOG_DEBUG)
{
    // Reserve space for short diagnostic bursts before allocation fault injection
    // is enabled by the caller, so capture does not consume the injected failure.
    if (output != nullptr) {
        output->reserve(output->size() + 8192);
    }
    CaptureScope capture(output, threshold);
    function();
    return true;
}
} // namespace npu_compute_test
#endif
