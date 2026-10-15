/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "hardware/hardware_device_api.h"

#include <acl/acl_rt.h>
#include <acl/acl_platform.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

#define CHECK(expression)                                      \
    do {                                                       \
        if (!(expression)) {                                   \
            ADD_FAILURE_AT(__FILE__, __LINE__) << #expression; \
            return false;                                      \
        }                                                      \
    } while (false)

constexpr std::int32_t kAclSuccess = 0;
constexpr std::int32_t kAclHbmMem = 1;
constexpr std::int32_t kHalModuleTypeCcpu = 2;
constexpr std::int32_t kHalModuleTypeAiCore = 4;
constexpr std::int32_t kHalModuleTypeVectorCore = 7;
constexpr std::int32_t kHalInfoTypeCoreNum = 3;
constexpr std::int32_t kHalInfoTypeFrequency = 4;
constexpr std::int32_t kHalInfoTypeCurrentFrequency = 32;
constexpr std::int32_t kHalErrorInvalidValue = 3;
constexpr std::int32_t kDsmiDeviceTypeHbm = 2;
constexpr std::size_t kDsmiTextLength = 32;
constexpr std::size_t kDsmiAiCpuCount = 16;

struct FakeDsmiChipInfo {
    unsigned char chipType[kDsmiTextLength];
    unsigned char chipName[kDsmiTextLength];
    unsigned char chipVersion[kDsmiTextLength];
};

struct FakeDsmiAiCpuInfo {
    unsigned int maxFrequency;
    unsigned int currentFrequency;
    unsigned int aiCpuCount;
    unsigned int utilization[kDsmiAiCpuCount];
};

struct Calls {
    std::vector<uint32_t> deviceInfoDeviceIds;
    std::vector<std::int32_t> deviceAttributes;
    std::vector<std::int32_t> platformTypes;
    std::vector<std::int32_t> setDeviceIds;
    std::vector<std::int32_t> memoryAttributes;
    std::vector<uint32_t> halDeviceIds;
    std::vector<std::int32_t> halModuleTypes;
    std::vector<std::int32_t> halInfoTypes;
    std::vector<std::int32_t> dsmiAiCpuDeviceIds;
    std::vector<std::int32_t> dsmiChipDeviceIds;
    std::vector<std::int32_t> dsmiFrequencyDeviceIds;
    std::vector<std::int32_t> dsmiDeviceTypes;
};

Calls g_calls;
std::int64_t g_oscFrequencyKhz = 1000000;
std::int32_t g_oscStatus = 0;
std::int64_t g_currentAicFrequency = 0;
std::int64_t g_currentAivFrequency = 0;
std::int64_t g_ratedAicFrequency = 1650;
std::int64_t g_ratedAivFrequency = 1650;

void ResetCalls() { g_calls = {}; }

extern "C" std::int32_t StubAclrtGetDeviceCount(uint32_t* value)
{
    *value = 2;
    return kAclSuccess;
}

extern "C" const char* StubAclrtGetSocName() { return "Ascend950PR_9599"; }

extern "C" std::int32_t StubAclrtGetDeviceInfo(uint32_t deviceId, std::int32_t attribute, std::int64_t* value)
{
    g_calls.deviceInfoDeviceIds.push_back(deviceId);
    g_calls.deviceAttributes.push_back(attribute);
    *value = 3510;
    return kAclSuccess;
}

extern "C" std::int32_t StubAclplatformGetDeviceInfo(std::int32_t type, char* value, uint32_t maxLength)
{
    g_calls.platformTypes.push_back(type);
    constexpr char kResult[] = "1800";
    if (maxLength < sizeof(kResult)) {
        return 1;
    }
    std::memcpy(value, kResult, sizeof(kResult));
    return kAclSuccess;
}

extern "C" std::int32_t StubAclrtSetDevice(std::int32_t deviceId)
{
    g_calls.setDeviceIds.push_back(deviceId);
    return kAclSuccess;
}

extern "C" std::int32_t StubAclrtGetMemInfo(std::int32_t attribute, std::size_t* freeBytes, std::size_t* totalBytes)
{
    g_calls.memoryAttributes.push_back(attribute);
    *freeBytes = 10U * 1024U * 1024U;
    *totalBytes = 16U * 1024U * 1024U;
    return kAclSuccess;
}

extern "C" std::int32_t StubHalGetDeviceInfo(
    uint32_t deviceId, std::int32_t moduleType, std::int32_t infoType, std::int64_t* value)
{
    g_calls.halDeviceIds.push_back(deviceId);
    g_calls.halModuleTypes.push_back(moduleType);
    g_calls.halInfoTypes.push_back(infoType);
    if (moduleType == 0 && infoType == 25) {
        *value = g_oscFrequencyKhz;
        return g_oscStatus;
    }
    if (moduleType == kHalModuleTypeCcpu && infoType == kHalInfoTypeCoreNum) {
        *value = 1;
        return 0;
    }
    if ((moduleType == kHalModuleTypeAiCore || moduleType == kHalModuleTypeVectorCore) &&
        infoType == kHalInfoTypeCurrentFrequency) {
        *value = moduleType == kHalModuleTypeAiCore ? g_currentAicFrequency : g_currentAivFrequency;
        return *value == 0 ? kHalErrorInvalidValue : 0;
    }
    if ((moduleType == kHalModuleTypeAiCore || moduleType == kHalModuleTypeVectorCore) &&
        infoType == kHalInfoTypeFrequency) {
        *value = moduleType == kHalModuleTypeAiCore ? g_ratedAicFrequency : g_ratedAivFrequency;
        return *value == 0 ? kHalErrorInvalidValue : 0;
    }
    return kHalErrorInvalidValue;
}

extern "C" std::int32_t StubDsmiGetAiCpuInfo(std::int32_t deviceId, FakeDsmiAiCpuInfo* value)
{
    g_calls.dsmiAiCpuDeviceIds.push_back(deviceId);
    value->currentFrequency = 1500;
    return 0;
}

extern "C" std::int32_t StubDsmiGetChipInfo(std::int32_t deviceId, FakeDsmiChipInfo* value)
{
    g_calls.dsmiChipDeviceIds.push_back(deviceId);
    constexpr char kVersion[] = "V100";
    std::memcpy(value->chipVersion, kVersion, sizeof(kVersion));
    return 0;
}

extern "C" std::int32_t StubDsmiGetDeviceFrequency(std::int32_t deviceId, std::int32_t deviceType, unsigned int* value)
{
    g_calls.dsmiFrequencyDeviceIds.push_back(deviceId);
    g_calls.dsmiDeviceTypes.push_back(deviceType);
    *value = 3200;
    return 0;
}

template <typename Function>
void* Symbol(Function function)
{
    return reinterpret_cast<void*>(function);
}

class FakeDynamicSymbolResolver final : public npucompute::DynamicSymbolResolver {
public:
    struct Library {
        std::string name;
    };

    void* FindLoadedSymbol(const char* name) override
    {
        requestedLoadedSymbols.emplace_back(name);
        const auto iterator = loadedSymbols.find(name);
        return iterator == loadedSymbols.end() ? nullptr : iterator->second;
    }

    void* OpenLibrary(const char* soname) override
    {
        openedLibraries.emplace_back(soname);
        if (availableLibraries.count(soname) == 0) {
            return nullptr;
        }
        auto library = std::make_unique<Library>();
        library->name = soname;
        void* handle = library.get();
        handles.emplace(handle, std::move(library));
        return handle;
    }

    void* FindLibrarySymbol(void* handle, const char* name) override
    {
        requestedLibrarySymbols.emplace_back(name);
        const auto handleIterator = handles.find(handle);
        if (handleIterator == handles.end()) {
            return nullptr;
        }
        const auto libraryIterator = librarySymbols.find(handleIterator->second->name);
        if (libraryIterator == librarySymbols.end()) {
            return nullptr;
        }
        const auto symbolIterator = libraryIterator->second.find(name);
        return symbolIterator == libraryIterator->second.end() ? nullptr : symbolIterator->second;
    }

    void CloseLibrary(void* handle) override
    {
        const auto iterator = handles.find(handle);
        if (iterator != handles.end()) {
            closedLibraries.push_back(iterator->second->name);
            handles.erase(iterator);
        }
    }

    void AddLoadedAclSymbols()
    {
        loadedSymbols = {
            {"aclrtGetDeviceCount", Symbol(&StubAclrtGetDeviceCount)},
            {"aclrtGetSocName", Symbol(&StubAclrtGetSocName)},
            {"aclrtGetDeviceInfo", Symbol(&StubAclrtGetDeviceInfo)},
            {"aclplatformGetDeviceInfo", Symbol(&StubAclplatformGetDeviceInfo)},
            {"aclrtSetDevice", Symbol(&StubAclrtSetDevice)},
            {"aclrtGetMemInfo", Symbol(&StubAclrtGetMemInfo)},
        };
    }

    void AddAclFallbackLibraries()
    {
        availableLibraries.insert("libascendcl.so");
        availableLibraries.insert("libplatform.so");
        librarySymbols["libascendcl.so"] = {
            {"aclrtGetDeviceCount", Symbol(&StubAclrtGetDeviceCount)},
            {"aclrtGetSocName", Symbol(&StubAclrtGetSocName)},
            {"aclrtGetDeviceInfo", Symbol(&StubAclrtGetDeviceInfo)},
            {"aclrtSetDevice", Symbol(&StubAclrtSetDevice)},
            {"aclrtGetMemInfo", Symbol(&StubAclrtGetMemInfo)},
        };
        librarySymbols["libplatform.so"] = {
            {"aclplatformGetDeviceInfo", Symbol(&StubAclplatformGetDeviceInfo)},
        };
    }

    void AddDriverLibraries()
    {
        availableLibraries.insert("libascend_hal.so");
        availableLibraries.insert("libdrvdsmi_host.so");
        librarySymbols["libascend_hal.so"] = {
            {"halGetDeviceInfo", Symbol(&StubHalGetDeviceInfo)},
        };
        librarySymbols["libdrvdsmi_host.so"] = {
            {"dsmi_get_aicpu_info", Symbol(&StubDsmiGetAiCpuInfo)},
            {"dsmi_get_chip_info", Symbol(&StubDsmiGetChipInfo)},
            {"dsmi_get_device_frequency", Symbol(&StubDsmiGetDeviceFrequency)},
        };
    }

    std::map<std::string, void*> loadedSymbols;
    std::set<std::string> availableLibraries;
    std::map<std::string, std::map<std::string, void*>> librarySymbols;
    std::map<void*, std::unique_ptr<Library>> handles;
    std::vector<std::string> requestedLoadedSymbols;
    std::vector<std::string> requestedLibrarySymbols;
    std::vector<std::string> openedLibraries;
    std::vector<std::string> closedLibraries;
};

bool Contains(const std::vector<std::string>& values, std::string_view expected)
{
    return std::find(values.begin(), values.end(), expected) != values.end();
}

bool TestSyscntFrequency()
{
    ResetCalls();
    auto resolver = std::make_shared<FakeDynamicSymbolResolver>();
    resolver->AddDriverLibraries();
    npucompute::DynamicHardwareDeviceApi api(resolver);
    double frequencyHz = 0;
    CHECK(api.GetSyscntFrequencyHz(3, &frequencyHz));
    CHECK(frequencyHz == 1000000000.0);
    CHECK(g_calls.halDeviceIds == std::vector<uint32_t>{3});
    CHECK(g_calls.halModuleTypes == std::vector<std::int32_t>{0});
    CHECK(g_calls.halInfoTypes == std::vector<std::int32_t>{25});
    for (const auto invalid : {0, -1}) {
        g_oscFrequencyKhz = invalid;
        CHECK(!api.GetSyscntFrequencyHz(3, &frequencyHz));
        CHECK(frequencyHz == 0);
    }
    g_oscFrequencyKhz = 1000000;
    g_oscStatus = kHalErrorInvalidValue;
    CHECK(!api.GetSyscntFrequencyHz(3, &frequencyHz));
    CHECK(frequencyHz == 0);
    g_oscStatus = 0;
    CHECK(!api.GetSyscntFrequencyHz(-1, &frequencyHz));
    CHECK(!api.GetSyscntFrequencyHz(3, nullptr));
    auto missing = std::make_shared<FakeDynamicSymbolResolver>();
    npucompute::DynamicHardwareDeviceApi missingApi(missing);
    CHECK(!missingApi.GetSyscntFrequencyHz(3, &frequencyHz));
    return true;
}

bool TestLoadedSymbolsAndExactArguments()
{
    ResetCalls();
    auto resolver = std::make_shared<FakeDynamicSymbolResolver>();
    resolver->AddLoadedAclSymbols();
    resolver->AddDriverLibraries();
    {
        npucompute::DynamicHardwareDeviceApi api(resolver);
        std::int32_t count = 0;
        std::string text;
        std::int64_t attributeValue = 0;
        uint32_t unsignedValue = 0;
        uint64_t freeBytes = 0;
        uint64_t totalBytes = 0;

        CHECK(api.GetDeviceCount(&count));
        CHECK(count == 2);
        CHECK(api.GetSocName(&text));
        CHECK(text == "Ascend950PR_9599");
        CHECK(api.GetDeviceAttribute(0, ACL_DEV_ATTR_NPU_ARCH, &attributeValue));
        CHECK(attributeValue == 3510);
        CHECK(api.GetPlatformValue(ACL_PLATFORM_CUBE_FREQ, &text));
        CHECK(text == "1800");
        CHECK(api.GetControlCpuCount(0, &unsignedValue));
        CHECK(unsignedValue == 1);
        uint32_t aicFrequency = 0;
        uint32_t aivFrequency = 0;
        CHECK(api.GetAiCoreFrequencies(0, &aicFrequency, &aivFrequency));
        CHECK(aicFrequency == 1650);
        CHECK(aivFrequency == 1650);
        CHECK(api.GetAiCpuFrequency(0, &unsignedValue));
        CHECK(unsignedValue == 1500);
        CHECK(api.GetChipVersion(0, &text));
        CHECK(text == "V100");
        CHECK(api.GetHbmUsage(0, &freeBytes, &totalBytes));
        CHECK(freeBytes == 10U * 1024U * 1024U);
        CHECK(totalBytes == 16U * 1024U * 1024U);
        CHECK(api.GetHbmFrequency(0, &unsignedValue));
        CHECK(unsignedValue == 3200);

        CHECK(g_calls.deviceInfoDeviceIds == std::vector<uint32_t>{0});
        CHECK(g_calls.deviceAttributes == std::vector<std::int32_t>{ACL_DEV_ATTR_NPU_ARCH});
        CHECK(g_calls.platformTypes == std::vector<std::int32_t>{ACL_PLATFORM_CUBE_FREQ});
        CHECK(g_calls.setDeviceIds == std::vector<std::int32_t>{0});
        CHECK(g_calls.memoryAttributes == std::vector<std::int32_t>{kAclHbmMem});
        CHECK((g_calls.halDeviceIds == std::vector<uint32_t>{0, 0, 0, 0, 0}));
        CHECK(
            g_calls.halModuleTypes == std::vector<std::int32_t>({
                                          kHalModuleTypeCcpu,
                                          kHalModuleTypeAiCore,
                                          kHalModuleTypeAiCore,
                                          kHalModuleTypeVectorCore,
                                          kHalModuleTypeVectorCore,
                                      }));
        CHECK(
            g_calls.halInfoTypes == std::vector<std::int32_t>({
                                        kHalInfoTypeCoreNum,
                                        kHalInfoTypeCurrentFrequency,
                                        kHalInfoTypeFrequency,
                                        kHalInfoTypeCurrentFrequency,
                                        kHalInfoTypeFrequency,
                                    }));
        CHECK(g_calls.dsmiAiCpuDeviceIds == std::vector<std::int32_t>{0});
        CHECK(g_calls.dsmiChipDeviceIds == std::vector<std::int32_t>{0});
        CHECK(g_calls.dsmiFrequencyDeviceIds == std::vector<std::int32_t>{0});
        CHECK(g_calls.dsmiDeviceTypes == std::vector<std::int32_t>{kDsmiDeviceTypeHbm});
        CHECK(!Contains(resolver->openedLibraries, "libascendcl.so"));
        CHECK(!Contains(resolver->openedLibraries, "libplatform.so"));
        CHECK(!Contains(resolver->requestedLoadedSymbols, "aclrtResetDevice"));
        CHECK(!Contains(resolver->requestedLibrarySymbols, "aclrtResetDevice"));
        g_calls.halInfoTypes.clear();
        CHECK(api.GetRatedAiCoreFrequencies(0, &aicFrequency, &aivFrequency));
        CHECK(aicFrequency == 1650);
        CHECK(aivFrequency == 1650);
        CHECK(g_calls.halInfoTypes == std::vector<std::int32_t>({kHalInfoTypeFrequency, kHalInfoTypeFrequency}));
    }
    CHECK(Contains(resolver->closedLibraries, "libascend_hal.so"));
    CHECK(Contains(resolver->closedLibraries, "libdrvdsmi_host.so"));
    return true;
}

bool TestSonameFallbackAndHandleLifetime()
{
    ResetCalls();
    auto resolver = std::make_shared<FakeDynamicSymbolResolver>();
    resolver->AddAclFallbackLibraries();
    {
        npucompute::DynamicHardwareDeviceApi api(resolver);
        std::int32_t count = 0;
        std::string value;
        std::int64_t attributeValue = 0;
        uint64_t freeBytes = 0;
        uint64_t totalBytes = 0;
        CHECK(api.GetDeviceCount(&count));
        CHECK(api.GetSocName(&value));
        CHECK(api.GetDeviceAttribute(0, ACL_DEV_ATTR_NPU_ARCH, &attributeValue));
        CHECK(api.GetPlatformValue(ACL_PLATFORM_MEMORY_SIZE, &value));
        CHECK(api.GetHbmUsage(0, &freeBytes, &totalBytes));
        CHECK(std::count(resolver->openedLibraries.begin(), resolver->openedLibraries.end(), "libascendcl.so") == 1);
        CHECK(std::count(resolver->openedLibraries.begin(), resolver->openedLibraries.end(), "libplatform.so") == 1);
        CHECK(resolver->closedLibraries.empty());
    }
    CHECK(Contains(resolver->closedLibraries, "libascendcl.so"));
    CHECK(Contains(resolver->closedLibraries, "libplatform.so"));
    return true;
}

bool TestMissingDriverLibrariesAreIndependent()
{
    ResetCalls();
    auto resolver = std::make_shared<FakeDynamicSymbolResolver>();
    resolver->AddLoadedAclSymbols();
    npucompute::DynamicHardwareDeviceApi api(resolver);
    uint32_t value = 0;
    std::string text;
    std::int32_t count = 0;

    CHECK(!api.GetControlCpuCount(0, &value));
    CHECK(!api.GetAiCoreFrequencies(0, &value, &value));
    CHECK(!api.GetAiCpuFrequency(0, &value));
    CHECK(!api.GetChipVersion(0, &text));
    CHECK(!api.GetHbmFrequency(0, &value));
    CHECK(api.GetDeviceCount(&count));
    CHECK(count == 2);
    CHECK(Contains(resolver->openedLibraries, "libascend_hal.so"));
    CHECK(Contains(resolver->openedLibraries, "libdrvdsmi_host.so"));
    return true;
}

bool TestMissingSingleAndAllSymbols()
{
    ResetCalls();
    auto partialResolver = std::make_shared<FakeDynamicSymbolResolver>();
    partialResolver->availableLibraries.insert("libascendcl.so");
    partialResolver->librarySymbols["libascendcl.so"] = {
        {"aclrtGetDeviceCount", Symbol(&StubAclrtGetDeviceCount)},
    };
    npucompute::DynamicHardwareDeviceApi partialApi(partialResolver);
    std::int32_t count = 0;
    std::int64_t attributeValue = 0;
    CHECK(partialApi.GetDeviceCount(&count));
    CHECK(!partialApi.GetDeviceAttribute(0, ACL_DEV_ATTR_NPU_ARCH, &attributeValue));
    CHECK(partialApi.GetDeviceCount(&count));

    auto emptyResolver = std::make_shared<FakeDynamicSymbolResolver>();
    npucompute::DynamicHardwareDeviceApi emptyApi(emptyResolver);
    std::string text;
    uint32_t value = 0;
    uint64_t freeBytes = 0;
    uint64_t totalBytes = 0;
    CHECK(!emptyApi.GetDeviceCount(&count));
    CHECK(!emptyApi.GetSocName(&text));
    CHECK(!emptyApi.GetDeviceAttribute(0, ACL_DEV_ATTR_NPU_ARCH, &attributeValue));
    CHECK(!emptyApi.GetPlatformValue(ACL_PLATFORM_MEMORY_SIZE, &text));
    CHECK(!emptyApi.GetControlCpuCount(0, &value));
    CHECK(!emptyApi.GetAiCoreFrequencies(0, &value, &value));
    CHECK(!emptyApi.GetAiCpuFrequency(0, &value));
    CHECK(!emptyApi.GetChipVersion(0, &text));
    CHECK(!emptyApi.GetHbmUsage(0, &freeBytes, &totalBytes));
    CHECK(!emptyApi.GetHbmFrequency(0, &value));
    return true;
}

} // namespace

static int RunSuiteMain()
{
    if (!TestSyscntFrequency() || !TestLoadedSymbolsAndExactArguments() || !TestSonameFallbackAndHandleLifetime() ||
        !TestMissingDriverLibrariesAreIndependent() || !TestMissingSingleAndAllSymbols()) {
        return 1;
    }
    return 0;
}

TEST(NpuComputeHardwareDeviceApi, Main)
{
    ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputeHardwareDeviceApi reported failure";
}

TEST(NpuComputeHardwareDeviceApi, CurrentFrequencyDoesNotFallBackToRated)
{
    auto resolver = std::make_shared<FakeDynamicSymbolResolver>();
    resolver->AddDriverLibraries();
    npucompute::DynamicHardwareDeviceApi api(resolver);
    uint32_t aic = 99;
    uint32_t aiv = 99;
    EXPECT_FALSE(api.GetCurrentAiCoreFrequencies(0, &aic, &aiv));
    EXPECT_EQ(aic, 0U);
    EXPECT_EQ(aiv, 0U);
    EXPECT_TRUE(api.GetRatedAiCoreFrequencies(0, &aic, &aiv));
    EXPECT_EQ(aic, 1650U);
    EXPECT_EQ(aiv, 1650U);
}

TEST(NpuComputeHardwareDeviceApi, CurrentFrequencyRetainsValidSide)
{
    auto resolver = std::make_shared<FakeDynamicSymbolResolver>();
    resolver->AddDriverLibraries();
    npucompute::DynamicHardwareDeviceApi api(resolver);
    g_currentAicFrequency = 1500;
    g_currentAivFrequency = -1;
    uint32_t aic = 99;
    uint32_t aiv = 99;
    EXPECT_FALSE(api.GetCurrentAiCoreFrequencies(0, &aic, &aiv));
    EXPECT_EQ(aic, 1500U);
    EXPECT_EQ(aiv, 0U);
    g_currentAicFrequency = 0;
    g_currentAivFrequency = 0;
}
