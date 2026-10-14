/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "acl/acl_rt.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" int aclrtInit();

namespace {

struct KernelArgs {
    uint8_t* value;
};

bool ParseInteger(const char* value, int* result)
{
    if (value == nullptr || result == nullptr || value[0] == '\0') {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == nullptr || end[0] != '\0' || parsed < 0 || parsed > 255) {
        return false;
    }
    *result = static_cast<int>(parsed);
    return true;
}

int ReleaseAndReturn(void* devPtr, int result)
{
    const int freeResult = aclrtFree(devPtr);
    if (freeResult != 0) {
        std::fprintf(stderr, "[demo] aclrtFree failed: %d\n", freeResult);
    }
    return result == 0 ? freeResult : result;
}

} // namespace

int main(int argc, char** argv)
{
    int sleepMilliseconds = 0;
    int requestedExitCode = 0;
    for (int index = 1; index < argc; ++index) {
        std::fprintf(stderr, "[demo] argv[%d]=%s\n", index, argv[index]);
        if (std::strcmp(argv[index], "--sleep-ms") == 0 && index + 1 < argc) {
            if (!ParseInteger(argv[++index], &sleepMilliseconds)) {
                std::fprintf(stderr, "[demo] invalid --sleep-ms value\n");
                return 2;
            }
            std::fprintf(stderr, "[demo] argv[%d]=%s\n", index, argv[index]);
        } else if (std::strcmp(argv[index], "--exit-code") == 0 && index + 1 < argc) {
            if (!ParseInteger(argv[++index], &requestedExitCode)) {
                std::fprintf(stderr, "[demo] invalid --exit-code value\n");
                return 2;
            }
            std::fprintf(stderr, "[demo] argv[%d]=%s\n", index, argv[index]);
        }
    }

    int result = aclrtInit();
    if (result != 0) {
        std::fprintf(stderr, "[demo] aclrtInit failed: %d\n", result);
        return result;
    }

    result = aclrtSetDevice(0);
    if (result != 0) {
        std::fprintf(stderr, "[demo] aclrtSetDevice failed: %d\n", result);
        return result;
    }

    void* devPtr = nullptr;
    result = aclrtMalloc(&devPtr, 4096, ACL_MEM_MALLOC_HUGE_FIRST);
    std::fprintf(stderr, "[demo] aclrtMalloc result=%d ptr=%p\n", result, devPtr);
    if (result != 0) {
        return result;
    }

    result = aclrtMemset(devPtr, 4096, 0, 4096);
    if (result != 0) {
        std::fprintf(stderr, "[demo] aclrtMemset failed: %d\n", result);
        return ReleaseAndReturn(devPtr, result);
    }

    std::uint8_t initialValue = 5;
    result = aclrtMemcpy(devPtr, 4096, &initialValue, 1, ACL_MEMCPY_HOST_TO_DEVICE);
    if (result != 0) {
        std::fprintf(stderr, "[demo] aclrtMemcpy H2D failed: %d\n", result);
        return ReleaseAndReturn(devPtr, result);
    }

    KernelArgs args{static_cast<std::uint8_t*>(devPtr)};
    result = aclrtLaunchKernel(nullptr, 1, &args, sizeof(args), nullptr);
    if (result != 0) {
        std::fprintf(stderr, "[demo] aclrtLaunchKernel failed: %d\n", result);
        return ReleaseAndReturn(devPtr, result);
    }

    std::uint8_t output = 0;
    result = aclrtMemcpy(&output, sizeof(output), devPtr, 1, ACL_MEMCPY_DEVICE_TO_HOST);
    if (result == 0 && output != 6) {
        std::fprintf(stderr, "[demo] unexpected kernel output: %u\n", static_cast<unsigned>(output));
        result = -1;
    }
    result = ReleaseAndReturn(devPtr, result);
    if (result != 0) {
        return result;
    }

    if (sleepMilliseconds > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMilliseconds));
    }
    if (requestedExitCode != 0) {
        std::fprintf(stderr, "[demo] exiting with requested status %d\n", requestedExitCode);
        return requestedExitCode;
    }
    std::fprintf(stderr, "[demo] completed\n");
    return 0;
}
