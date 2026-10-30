/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "acl_pti/profiling/replay_runtime.h"
#include "injection/runtime_stub_api.h"
#include <cstdio>
#include <cstdlib>

namespace {
int companionLoads = 0;
int token;
aclError Load(const void*, std::size_t, const aclrtBinaryLoadOptions*, aclrtBinHandle* handle)
{
    ++companionLoads;
    *handle = &token;
    return ACL_SUCCESS;
}
} // namespace
// No profiling round is part of registration.
extern "C" std::int32_t MsprofStart(std::uint32_t, const void*, std::uint32_t) { return -1; } // namespace
extern "C" std::int32_t MsprofStop(std::uint32_t, const void*, std::uint32_t) { return -1; }
extern "C" std::int32_t MsprofRegisterDataCallback(std::uint32_t, void*) { return -1; }

int main()
{
    using aclpti::profiling::ReplayRuntime;
    if (RuntimeStubSetOriginFunction("aclrtBinaryLoadFromData", &Load) != ACL_SUCCESS) {
        return 1;
    }
    for (const auto mode : {0, 1, 2, 3}) {
        ReplayRuntime runtime;
        aclptiRangeProfilerSetConfigParams config{};
        const char* section = "Memory";
        if (mode == 0) {
            config.numSections = 1;
            config.sections = &section;
        }
        config.collectPipeline = mode == 1 || mode == 3;
        config.collectPcSampling = mode == 2 || mode == 3;
        if (runtime.SetConfig(&config) != ACLPTI_SUCCESS) {
            return 2;
        }
        int binary;
        const char image[] = "ELF";
        const auto before = companionLoads;
        const int expectedLoads = mode == 0 ? 0 : (mode == 3 ? 2 : 1);
        const auto firstRegistration = runtime.RegisterBinary(image, sizeof(image), nullptr, &binary);
        const auto secondRegistration = runtime.RegisterBinary(image, sizeof(image), nullptr, &binary);
        if (firstRegistration != ACLPTI_SUCCESS || secondRegistration != ACLPTI_SUCCESS) {
            std::fprintf(
                stderr, "mode=%d register status=%d/%d loads=%d expected=%d\n", mode, firstRegistration,
                secondRegistration, companionLoads - before, expectedLoads);
            return 3;
        }
        if (companionLoads - before != expectedLoads) {
            return 4;
        }
        if (mode != 0 && runtime.RegisterBinaryFunction(&binary, "kernel", &token) != ACLPTI_SUCCESS) {
            return 5;
        }
    }
    if (setenv("NPU_COMPUTE_TEST_PM_MAP_FAILURE", "1", 1) != 0) {
        return 6;
    }
    for (const bool pipeline : {false, true}) {
        ReplayRuntime runtime;
        aclptiRangeProfilerSetConfigParams config{};
        config.collectPcSampling = true;
        config.collectPipeline = pipeline;
        if (runtime.SetConfig(&config) != ACLPTI_SUCCESS) {
            return 7;
        }
        int firstBinary, secondBinary;
        const char image[] = "ELF";
        const auto before = companionLoads;
        if (runtime.RegisterBinary(image, sizeof(image), nullptr, &firstBinary) != ACLPTI_SUCCESS ||
            runtime.RegisterBinaryFunction(&firstBinary, "kernel", &token) != ACLPTI_SUCCESS ||
            runtime.RegisterBinary(image, sizeof(image), nullptr, &secondBinary) != ACLPTI_SUCCESS ||
            companionLoads != before + (pipeline ? 2 : 0)) {
            return 8;
        }
        aclpti::profiling::PmSamplingBinarySnapshot snapshot;
        if (!runtime.GetPmSamplingSnapshot(&token, snapshot) || snapshot.error.empty() || !snapshot.image ||
            snapshot.image->originalElf.empty()) {
            return 9;
        }
    }
    if (unsetenv("NPU_COMPUTE_TEST_PM_MAP_FAILURE") != 0) {
        return 10;
    }
    std::puts("PASS: registration gates, Pipeline companions, mapping failure keeps profiling available");
}
