// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.
#include "acl_san/aclsan_api.h"
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"
#include "kernel_argument_elf_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

#define CHECK(expr)                                                 \
    do {                                                            \
        if (!(expr)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); \
            return 1;                                               \
        }                                                           \
    } while (false)

namespace {
size_t fileCalls = 0, dataCalls = 0, allocations = 0;
bool delegateFile = false, delegateMalloc = false, failMalloc = false, failUnload = false, failLoad = false;
std::string loadedPath;
std::vector<uint8_t> loadedImage;
const aclrtBinaryLoadOptions* receivedOptions = nullptr;
aclrtMallocConfig* receivedConfig = nullptr;
AclsanResourceData resource{};
std::string resourceApi;
const auto binaryHandle = reinterpret_cast<void*>(0x100);
const auto functionHandle = reinterpret_cast<void*>(0x200);
const auto memory = reinterpret_cast<void*>(0x12340000);

aclError Data(const void* data, size_t bytes, const aclrtBinaryLoadOptions* options, aclrtBinHandle* binary)
{
    ++dataCalls;
    loadedImage.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    receivedOptions = options;
    *binary = binaryHandle;
    return ACL_SUCCESS;
}
aclError File(const char* path, aclrtBinaryLoadOptions* options, aclrtBinHandle* binary)
{
    ++fileCalls;
    loadedPath = path;
    receivedOptions = options;
    if (failLoad)
        return 77;
    std::ifstream input(path, std::ios::binary);
    loadedImage.assign(std::istreambuf_iterator<char>(input), {});
    if (loadedImage.empty())
        return 78;
    *binary = binaryHandle;
    if (delegateFile)
        return aclrtBinaryLoadFromData(loadedImage.data(), loadedImage.size(), options, binary);
    return ACL_SUCCESS;
}
aclError Lookup(aclrtBinHandle, const char*, aclrtFuncHandle* function)
{
    *function = functionHandle;
    return ACL_SUCCESS;
}
aclError Unload(aclrtBinHandle) { return failUnload ? 79 : ACL_SUCCESS; }
aclError Device(int32_t* device)
{
    *device = 3;
    return ACL_SUCCESS;
}
aclError Malloc(void** ptr, size_t, aclrtMemMallocPolicy)
{
    *ptr = memory;
    return ACL_SUCCESS;
}
aclError MallocCfg(void** ptr, size_t bytes, aclrtMemMallocPolicy policy, aclrtMallocConfig* cfg)
{
    receivedConfig = cfg;
    if (failMalloc)
        return 80;
    if (delegateMalloc)
        return aclrtMalloc(ptr, bytes, policy);
    *ptr = memory;
    return ACL_SUCCESS;
}
void Callback(void*, AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    if (domain == ACLSAN_CB_DOMAIN_RESOURCE && id == ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) {
        ++allocations;
        resource = *static_cast<const AclsanResourceData*>(data);
        resourceApi = resource.common.apiName;
    }
}
} // namespace

int main()
{
#define REGISTER(name, fn) CHECK(RuntimeStubSetOriginFunction(name, &fn) == ACL_SUCCESS)
    REGISTER("aclrtBinaryLoadFromFile", File);
    REGISTER("aclrtBinaryLoadFromData", Data);
    REGISTER("aclrtBinaryGetFunction", Lookup);
    REGISTER("aclrtBinaryUnLoad", Unload);
    REGISTER("aclrtMallocWithCfg", MallocCfg);
    REGISTER("aclrtMalloc", Malloc);
    REGISTER("aclrtGetDevice", Device);
    CHECK(acltoolHookInit() == ACL_SUCCESS);
    AclsanSubscriberHandle subscriber{};
    CHECK(aclsanSubscribe(&subscriber, Callback, nullptr) == ACLSAN_STATUS_SUCCESS);
    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) ==
        ACLSAN_STATUS_SUCCESS);
    aclrtMallocConfig config{};
    void* ptr = nullptr;
    for (bool delegate : {false, true}) {
        delegateMalloc = delegate;
        const auto count = allocations;
        CHECK(aclrtMallocWithCfg(&ptr, 123, ACL_MEM_MALLOC_HUGE_FIRST, &config) == ACL_SUCCESS);
        CHECK(allocations == count + 1 && receivedConfig == &config);
        CHECK(
            resourceApi == "aclrtMallocWithCfg" && resource.ptr == memory && resource.bytes == 123 &&
            resource.deviceId == 3);
    }
    failMalloc = true;
    CHECK(aclrtMallocWithCfg(&ptr, 456, ACL_MEM_MALLOC_HUGE_FIRST, nullptr) == 80);
    CHECK(resource.common.result == 80 && resource.ptr == nullptr && resource.bytes == 456);
    CHECK(ptr == memory && receivedConfig == nullptr);
    failMalloc = false;
    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC) ==
        ACLSAN_STATUS_SUCCESS);
    char path[] = "/tmp/aclsan-file-test-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0);
    const auto image = aclsan::test::MakeKernelArgumentSizeElf(16);
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()), image.size());
        CHECK(output.good());
    }
    aclrtBinaryLoadOption option{};
    option.type = ACL_RT_BINARY_LOAD_OPT_LAZY_LOAD;
    option.value.isLazyLoad = 0;
    aclrtBinaryLoadOptions options{&option, 1};
    aclrtBinHandle binary{};
    for (bool delegate : {false, true}) {
        delegateFile = delegate;
        const auto previousFiles = fileCalls;
        const auto previousData = dataCalls;
        CHECK(aclrtBinaryLoadFromFile(path, &options, &binary) == ACL_SUCCESS);
        CHECK(fileCalls == previousFiles + 1 && dataCalls == previousData + (delegate ? 1 : 0));
        CHECK(receivedOptions == &options && option.value.isLazyLoad == 0);
        CHECK(loadedPath != path && access(loadedPath.c_str(), F_OK) == 0);
        CHECK(loadedImage != image); // Existing production metadata repair ran on the transformed file.
        std::ifstream original(path, std::ios::binary);
        const std::vector<uint8_t> preserved{std::istreambuf_iterator<char>(original), {}};
        CHECK(preserved == image);
        failUnload = true;
        CHECK(aclrtBinaryUnLoad(binary) == 79 && access(loadedPath.c_str(), F_OK) == 0);
        failUnload = false;
        CHECK(aclrtBinaryUnLoad(binary) == ACL_SUCCESS && access(loadedPath.c_str(), F_OK) != 0);
    }
    failLoad = true;
    CHECK(aclrtBinaryLoadFromFile(path, nullptr, &binary) == 77);
    CHECK(access(loadedPath.c_str(), F_OK) != 0);
    failLoad = false;
    CHECK(aclrtBinaryLoadFromFile(nullptr, nullptr, &binary) == ACL_ERROR_INVALID_PARAM);
    CHECK(aclrtBinaryLoadFromFile(path, nullptr, nullptr) == ACL_ERROR_INVALID_PARAM);
    CHECK(unlink(path) == 0);
    const auto previousFiles = fileCalls;
    CHECK(aclrtBinaryLoadFromFile(path, nullptr, &binary) != ACL_SUCCESS && fileCalls == previousFiles);
    CHECK(aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS);
    failLoad = true;
    CHECK(aclrtBinaryLoadFromFile(path, nullptr, &binary) == 77 && loadedPath == path);
    const auto count = allocations;
    CHECK(aclrtMallocWithCfg(&ptr, 789, ACL_MEM_MALLOC_HUGE_FIRST, nullptr) == ACL_SUCCESS);
    CHECK(allocations == count);
    std::puts("ACLNN_RUNTIME_HOOKS_HOST_PASS");
    return 0;
}
