/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "aclpti/aclpti.h"
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "check failed at %d: %s\n", __LINE__, #condition); \
            return 1;                                                               \
        }                                                                           \
    } while (false)

namespace {
std::vector<aclrtMemMallocPolicy> mallocPolicies;
std::vector<aclrtMemMallocPolicy> alignedPolicies;
std::vector<aclrtMemMallocPolicy> configuredPolicies;
std::vector<aclrtMallocConfig*> configs;
std::size_t mallocCalls = 0;
std::size_t freeCalls = 0;
std::size_t shutdownCalls = 0;
bool failShadow = false;
bool failBusiness = false;
aclError Malloc(void** pointer, std::size_t size, aclrtMemMallocPolicy policy)
{
    ++mallocCalls;
    mallocPolicies.push_back(policy);
    if (failBusiness || (failShadow && mallocCalls == 2)) {
        return ACL_ERROR_BAD_ALLOC;
    }
    *pointer = std::malloc(size);
    return *pointer ? ACL_SUCCESS : ACL_ERROR_BAD_ALLOC;
}
aclError Aligned(void** pointer, std::size_t size, aclrtMemMallocPolicy policy)
{
    alignedPolicies.push_back(policy);
    *pointer = std::malloc(size);
    return *pointer ? ACL_SUCCESS : ACL_ERROR_BAD_ALLOC;
}
aclError Configured(void** pointer, std::size_t size, aclrtMemMallocPolicy policy, aclrtMallocConfig* cfg)
{
    configuredPolicies.push_back(policy);
    configs.push_back(cfg);
    *pointer = std::malloc(size);
    return *pointer ? ACL_SUCCESS : ACL_ERROR_BAD_ALLOC;
}
aclError Free(void* pointer)
{
    ++freeCalls;
    std::free(pointer);
    return ACL_SUCCESS;
}
aclptiResult Shutdown(void*)
{
    ++shutdownCalls;
    return ACLPTI_SUCCESS;
}
} // namespace

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const std::string_view scenario(argv[1]);
    const bool none = scenario != "all" && scenario != "business_failure";
    CHECK(RuntimeStubSetOriginFunction("aclrtMalloc", &Malloc) == 0);
    CHECK(RuntimeStubSetOriginFunction("aclrtMallocAlign32", &Aligned) == 0);
    CHECK(RuntimeStubSetOriginFunction("aclrtMallocWithCfg", &Configured) == 0);
    CHECK(RuntimeStubSetOriginFunction("aclrtFree", &Free) == 0);
    aclptiSubscribeHandle subscriber = nullptr;
    CHECK(aclptiSubscribe(&subscriber, nullptr, nullptr, nullptr) == ACLPTI_SUCCESS);
    const char* sections[] = {"PipeUtilization"};
    aclptiRangeProfilerSetConfigParams config{sections, 1};
    config.cacheControl = none ? ACLPTI_CACHE_CONTROL_NONE : ACLPTI_CACHE_CONTROL_ALL;
    CHECK(aclptiRangeProfilerSetConfig(&config) == ACLPTI_SUCCESS);
    // Reject an invalid section configuration.
    const char* invalidSections[] = {"UnknownSection"};
    auto invalidConfig = config;
    invalidConfig.sections = invalidSections;
    invalidConfig.cacheControl = none ? ACLPTI_CACHE_CONTROL_ALL : ACLPTI_CACHE_CONTROL_NONE;
    CHECK(aclptiRangeProfilerSetConfig(&invalidConfig) == ACLPTI_ERROR_NOT_SUPPORTED);
    CHECK(aclptiRegisterDataModuleShutdownCallback(&Shutdown, nullptr) == ACLPTI_SUCCESS);
    void* pointer = nullptr;
    if (scenario == "business_failure") {
        failBusiness = true;
        CHECK(aclrtMalloc(&pointer, 8, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_ERROR_BAD_ALLOC);
        CHECK(pointer == nullptr && mallocCalls == 1 && shutdownCalls == 1);
        return 0;
    }
    if (scenario == "shadow_failure") {
        failShadow = true;
        CHECK(aclrtMalloc(&pointer, 8, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_ERROR_PROFILING_FAILURE);
        CHECK(pointer != nullptr && shutdownCalls == 1);
        CHECK(aclrtFree(pointer) == ACL_SUCCESS);
        CHECK(freeCalls == 1);
        return 0;
    }
    const aclrtMemMallocPolicy policies[] = {
        ACL_MEM_MALLOC_HUGE_FIRST,      ACL_MEM_MALLOC_HUGE_ONLY,       ACL_MEM_MALLOC_NORMAL_ONLY,
        ACL_MEM_MALLOC_HUGE1G_ONLY,     ACL_MEM_MALLOC_HUGE_FIRST_P2P,  ACL_MEM_MALLOC_HUGE_ONLY_P2P,
        ACL_MEM_MALLOC_NORMAL_ONLY_P2P, ACL_MEM_MALLOC_HUGE1G_ONLY_P2P,
    };
    aclrtMallocAttribute attribute{};
    attribute.attr = ACL_RT_MEM_ATTR_MODULE_ID;
    attribute.value.moduleId = 1;
    aclrtMallocConfig mallocConfig{&attribute, 1};
    for (std::size_t index = 0; index < 8; ++index) {
        const auto policy = policies[index];
        CHECK(aclrtMalloc(&pointer, 8, policy) == ACL_SUCCESS);
        CHECK(mallocPolicies.size() == (index + 1) * 2);
        CHECK(mallocPolicies[index * 2] == policy && mallocPolicies[index * 2 + 1] == policy);
        CHECK(aclrtFree(pointer) == ACL_SUCCESS);

        CHECK(aclrtMallocAlign32(&pointer, 8, policy) == ACL_SUCCESS);
        CHECK(alignedPolicies.size() == (index + 1) * 2);
        CHECK(alignedPolicies[index * 2] == policy && alignedPolicies[index * 2 + 1] == policy);
        CHECK(aclrtFree(pointer) == ACL_SUCCESS);

        CHECK(aclrtMallocWithCfg(&pointer, 8, policy, &mallocConfig) == ACL_SUCCESS);
        CHECK(configuredPolicies.size() == (index + 1) * 2);
        CHECK(configuredPolicies[index * 2] == policy && configuredPolicies[index * 2 + 1] == policy);
        CHECK(configs[index * 2] == &mallocConfig && configs[index * 2 + 1] == &mallocConfig);
        CHECK(aclrtFree(pointer) == ACL_SUCCESS);
    }
    CHECK(mallocCalls == 16 && freeCalls == 48);
    CHECK(aclrtMallocWithCfg(&pointer, 8, ACL_MEM_MALLOC_HUGE_FIRST, nullptr) == ACL_SUCCESS);
    CHECK(configs.size() == 18 && configs[16] == nullptr && configs[17] == nullptr);
    CHECK(aclrtFree(pointer) == ACL_SUCCESS);
    // Allocations no longer determine cache-control, so configuration can change before replay.
    config.cacheControl = none ? ACLPTI_CACHE_CONTROL_ALL : ACLPTI_CACHE_CONTROL_NONE;
    CHECK(aclptiRangeProfilerSetConfig(&config) == ACLPTI_SUCCESS);
    return 0;
}
