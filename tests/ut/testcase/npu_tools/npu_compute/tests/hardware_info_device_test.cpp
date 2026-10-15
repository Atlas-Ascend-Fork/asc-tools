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

#include "hardware/hardware_info_device.h"

#include <acl/acl_rt.h>
#include <acl/acl_platform.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
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

class FakeHardwareDeviceApi final : public npucompute::HardwareDeviceApi {
public:
    bool GetDeviceCount(std::int32_t* value) override
    {
        ++deviceCountCalls;
        if (failDeviceCount) {
            return false;
        }
        *value = deviceCount;
        return true;
    }

    bool GetSocName(std::string* value) override
    {
        ++socNameCalls;
        if (failSocName) {
            return false;
        }
        *value = socName;
        return true;
    }

    bool GetDeviceAttribute(std::int32_t deviceId, std::int32_t attribute, std::int64_t* value) override
    {
        deviceIds.push_back(deviceId);
        deviceAttributes.push_back(attribute);
        if (failedDeviceAttributes.count(attribute) != 0) {
            return false;
        }
        const auto iterator = deviceAttributeValues.find(attribute);
        if (iterator == deviceAttributeValues.end()) {
            return false;
        }
        *value = iterator->second;
        return true;
    }

    bool GetPlatformValue(std::int32_t type, std::string* value) override
    {
        platformTypes.push_back(type);
        if (failedPlatformTypes.count(type) != 0) {
            return false;
        }
        const auto iterator = platformValues.find(type);
        if (iterator == platformValues.end()) {
            return false;
        }
        *value = iterator->second;
        return true;
    }

    bool GetControlCpuCount(std::int32_t deviceId, uint32_t* value) override
    {
        deviceIds.push_back(deviceId);
        if (failControlCpuCount) {
            return false;
        }
        *value = controlCpuCount;
        return true;
    }

    bool GetAiCpuFrequency(std::int32_t deviceId, uint32_t* value) override
    {
        deviceIds.push_back(deviceId);
        if (failAiCpuFrequency) {
            return false;
        }
        *value = aiCpuFrequency;
        return true;
    }

    bool GetAiCoreFrequencies(std::int32_t deviceId, uint32_t* aicValue, uint32_t* aivValue) override
    {
        deviceIds.push_back(deviceId);
        if (failAiCoreFrequencies) {
            return false;
        }
        *aicValue = aicFrequency;
        *aivValue = aivFrequency;
        return true;
    }

    bool GetCurrentAiCoreFrequencies(std::int32_t, uint32_t* aicValue, uint32_t* aivValue) override
    {
        *aicValue = currentAicFrequency;
        *aivValue = currentAivFrequency;
        return currentAicFrequency != 0 && currentAivFrequency != 0;
    }

    bool GetRatedAiCoreFrequencies(std::int32_t, uint32_t* aicValue, uint32_t* aivValue) override
    {
        *aicValue = ratedAicFrequency;
        *aivValue = ratedAivFrequency;
        return ratedAicFrequency != 0 && ratedAivFrequency != 0;
    }

    uint32_t currentAicFrequency = 0;
    uint32_t currentAivFrequency = 0;
    uint32_t ratedAicFrequency = 0;
    uint32_t ratedAivFrequency = 0;

    bool GetChipVersion(std::int32_t deviceId, std::string* value) override
    {
        deviceIds.push_back(deviceId);
        if (failChipVersion) {
            return false;
        }
        *value = chipVersion;
        return true;
    }

    bool GetHbmUsage(std::int32_t deviceId, uint64_t* freeBytes, uint64_t* totalBytes) override
    {
        deviceIds.push_back(deviceId);
        if (failHbmUsage) {
            return false;
        }
        *freeBytes = hbmFreeBytes;
        *totalBytes = hbmTotalBytes;
        return true;
    }

    bool GetHbmFrequency(std::int32_t deviceId, uint32_t* value) override
    {
        deviceIds.push_back(deviceId);
        if (failHbmFrequency) {
            return false;
        }
        *value = hbmFrequency;
        return true;
    }

    std::size_t SpecializedCallCount() const
    {
        return static_cast<std::size_t>(socNameCalls) + deviceIds.size() + deviceAttributes.size() +
               platformTypes.size();
    }

    std::int32_t deviceCount = 1;
    std::string socName = "Ascend950PR_9599";
    std::string chipVersion = "V100";
    uint32_t controlCpuCount = 1;
    uint32_t aiCpuFrequency = 1500;
    uint32_t aicFrequency = 1650;
    uint32_t aivFrequency = 1650;
    uint64_t hbmFreeBytes = 10ULL * 1024ULL * 1024ULL;
    uint64_t hbmTotalBytes = 16ULL * 1024ULL * 1024ULL;
    uint32_t hbmFrequency = 3200;
    std::map<std::int32_t, std::int64_t> deviceAttributeValues = {
        {ACL_DEV_ATTR_NPU_ARCH, 3510},    {ACL_DEV_ATTR_AICPU_CORE_NUM, 6},   {ACL_DEV_ATTR_AICORE_CORE_NUM, 36},
        {ACL_DEV_ATTR_CUBE_CORE_NUM, 36}, {ACL_DEV_ATTR_VECTOR_CORE_NUM, 72},
    };
    std::map<std::int32_t, std::string> platformValues = {
        {ACL_PLATFORM_MEMORY_SIZE, "137438953472"},
        {ACL_PLATFORM_CUBE_FREQ, "1800"},
        {ACL_PLATFORM_VEC_FREQ, "1700"},
    };

    bool failDeviceCount = false;
    bool failSocName = false;
    bool failControlCpuCount = false;
    bool failAiCpuFrequency = false;
    bool failAiCoreFrequencies = false;
    bool failChipVersion = false;
    bool failHbmUsage = false;
    bool failHbmFrequency = false;
    std::set<std::int32_t> failedDeviceAttributes;
    std::set<std::int32_t> failedPlatformTypes;

    int deviceCountCalls = 0;
    int socNameCalls = 0;
    std::vector<std::int32_t> deviceIds;
    std::vector<std::int32_t> deviceAttributes;
    std::vector<std::int32_t> platformTypes;
};

bool Contains(const std::vector<std::string>& diagnostics, std::string_view text)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [text](const std::string& value) {
        return value.find(text) != std::string::npos;
    });
}

bool TestCompleteMapping()
{
    FakeHardwareDeviceApi api;
    npucompute::DeviceInfo device;
    npucompute::CpuInfo cpu;
    npucompute::AiCoreInfo aiCore;
    npucompute::MemoryInfo memory;
    std::vector<std::string> diagnostics;
    npucompute::DiagnosticSink sink = [&diagnostics](std::string_view value) { diagnostics.emplace_back(value); };

    CHECK(npucompute::CollectDevice0Info(api, &device, &cpu, &aiCore, &memory, &sink));
    CHECK(device.npuCount == 1);
    CHECK(device.chipInfo == "Ascend950PR_9599 V100");
    CHECK(device.archInfo == "3510");
    CHECK(cpu.controlCpuCount == 1);
    CHECK(cpu.aiCpuCount == 6);
    CHECK(cpu.aiCpuFrequencyMhz == 1500);
    CHECK(aiCore.aiCoreCount == 36);
    CHECK(aiCore.aiCubeCount == 36);
    CHECK(aiCore.aiVectorCount == 72);
    CHECK(aiCore.aiCubeFrequencyMhz == 1650);
    CHECK(aiCore.aiVectorFrequencyMhz == 1650);
    CHECK(memory.hbmTotalMb == 131072.0);
    CHECK(memory.hbmUsedMb == 6.0);
    CHECK(memory.hbmFrequencyMhz == 3200);
    CHECK(diagnostics.empty());

    const std::vector<std::int32_t> expectedAttributes = {
        ACL_DEV_ATTR_NPU_ARCH,      ACL_DEV_ATTR_AICPU_CORE_NUM,  ACL_DEV_ATTR_AICORE_CORE_NUM,
        ACL_DEV_ATTR_CUBE_CORE_NUM, ACL_DEV_ATTR_VECTOR_CORE_NUM,
    };
    const std::vector<std::int32_t> expectedPlatformTypes = {ACL_PLATFORM_MEMORY_SIZE};
    CHECK(api.deviceAttributes == expectedAttributes);
    CHECK(api.platformTypes == expectedPlatformTypes);
    CHECK(!api.deviceIds.empty());
    CHECK(std::all_of(api.deviceIds.begin(), api.deviceIds.end(), [](std::int32_t value) { return value == 0; }));
    return true;
}

bool TestCollectAiCoreCountsOnlyReadsCountAttributes()
{
    FakeHardwareDeviceApi api;
    std::uint32_t cubeCount = 0;
    std::uint32_t vectorCount = 0;
    std::vector<std::string> diagnostics;
    npucompute::DiagnosticSink sink = [&diagnostics](std::string_view value) { diagnostics.emplace_back(value); };

    CHECK(npucompute::CollectAiCoreCounts(api, &cubeCount, &vectorCount, &sink));
    CHECK(cubeCount == 36);
    CHECK(vectorCount == 72);
    const std::vector<std::int32_t> expectedAttributes = {
        ACL_DEV_ATTR_CUBE_CORE_NUM,
        ACL_DEV_ATTR_VECTOR_CORE_NUM,
    };
    CHECK(api.deviceAttributes == expectedAttributes);
    const std::vector<std::int32_t> expectedDeviceIds = {0, 0};
    CHECK(api.deviceIds == expectedDeviceIds);
    CHECK(api.deviceCountCalls == 0);
    CHECK(api.socNameCalls == 0);
    CHECK(api.platformTypes.empty());
    CHECK(diagnostics.empty());
    return true;
}

bool TestCollectAiCoreFrequencies()
{
    FakeHardwareDeviceApi api;
    api.aicFrequency = 1800;
    api.aivFrequency = 1700;
    std::uint32_t cubeFrequency = 99;
    std::uint32_t vectorFrequency = 99;
    std::vector<std::string> diagnostics;
    npucompute::DiagnosticSink sink = [&diagnostics](std::string_view value) { diagnostics.emplace_back(value); };

    CHECK(npucompute::CollectAiCoreFrequencies(api, &cubeFrequency, &vectorFrequency, &sink));
    CHECK(cubeFrequency == 1800);
    CHECK(vectorFrequency == 1700);
    CHECK(api.deviceCountCalls == 0);
    CHECK(api.socNameCalls == 0);
    CHECK(api.deviceAttributes.empty());
    CHECK(api.deviceIds == std::vector<std::int32_t>({0}));
    CHECK(diagnostics.empty());

    api = FakeHardwareDeviceApi{};
    diagnostics.clear();
    cubeFrequency = 99;
    vectorFrequency = 99;
    api.failAiCoreFrequencies = true;
    CHECK(!npucompute::CollectAiCoreFrequencies(api, &cubeFrequency, &vectorFrequency, &sink));
    CHECK(cubeFrequency == 1650);
    CHECK(vectorFrequency == 1650);
    CHECK(Contains(diagnostics, "GetAiCoreFrequencies"));

    api = FakeHardwareDeviceApi{};
    diagnostics.clear();
    CHECK(!npucompute::CollectAiCoreFrequencies(api, nullptr, &vectorFrequency, &sink));
    CHECK(api.platformTypes.empty());
    CHECK(Contains(diagnostics, "output is null"));
    return true;
}

bool TestPartialFailuresAndInvalidValues()
{
    FakeHardwareDeviceApi api;
    api.failSocName = true;
    api.failedDeviceAttributes.insert(ACL_DEV_ATTR_AICORE_CORE_NUM);
    api.deviceAttributeValues[ACL_DEV_ATTR_AICPU_CORE_NUM] = -1;
    api.failAiCoreFrequencies = true;
    api.platformValues[ACL_PLATFORM_MEMORY_SIZE] = "not-bytes";
    api.hbmFreeBytes = 20;
    api.hbmTotalBytes = 10;
    api.failHbmFrequency = true;

    npucompute::DeviceInfo device;
    npucompute::CpuInfo cpu;
    npucompute::AiCoreInfo aiCore;
    npucompute::MemoryInfo memory;
    std::vector<std::string> diagnostics;
    npucompute::DiagnosticSink sink = [&diagnostics](std::string_view value) { diagnostics.emplace_back(value); };

    CHECK(npucompute::CollectDevice0Info(api, &device, &cpu, &aiCore, &memory, &sink));
    CHECK(device.npuCount == 1);
    CHECK(device.chipInfo.empty());
    CHECK(device.archInfo == "3510");
    CHECK(cpu.controlCpuCount == 1);
    CHECK(cpu.aiCpuCount == 0);
    CHECK(cpu.aiCpuFrequencyMhz == 1500);
    CHECK(aiCore.aiCoreCount == 0);
    CHECK(aiCore.aiCubeCount == 36);
    CHECK(aiCore.aiVectorCount == 72);
    CHECK(aiCore.aiCubeFrequencyMhz == 1650);
    CHECK(aiCore.aiVectorFrequencyMhz == 1650);
    CHECK(memory.hbmTotalMb == 0);
    CHECK(memory.hbmUsedMb == 0);
    CHECK(memory.hbmFrequencyMhz == 0);
    CHECK(Contains(diagnostics, "GetSocName"));
    CHECK(Contains(diagnostics, "AI CPU core count"));
    CHECK(Contains(diagnostics, "AI Core count"));
    CHECK(Contains(diagnostics, "GetAiCoreFrequencies"));
    CHECK(Contains(diagnostics, "HBM total"));
    CHECK(Contains(diagnostics, "HBM usage"));
    CHECK(Contains(diagnostics, "GetHbmFrequency"));
    return true;
}

bool TestNoVisibleDeviceSkipsDeviceQueries()
{
    FakeHardwareDeviceApi api;
    api.deviceCount = 0;
    npucompute::DeviceInfo device;
    npucompute::CpuInfo cpu;
    npucompute::AiCoreInfo aiCore;
    npucompute::MemoryInfo memory;

    CHECK(npucompute::CollectDevice0Info(api, &device, &cpu, &aiCore, &memory, nullptr));
    CHECK(api.deviceCountCalls == 1);
    CHECK(api.SpecializedCallCount() == 0);
    CHECK(device.npuCount == 0);
    return true;
}

bool TestInvalidDeviceCountAndOutputPointers()
{
    FakeHardwareDeviceApi api;
    api.deviceCount = -1;
    npucompute::DeviceInfo device;
    npucompute::CpuInfo cpu;
    npucompute::AiCoreInfo aiCore;
    npucompute::MemoryInfo memory;
    std::vector<std::string> diagnostics;
    npucompute::DiagnosticSink sink = [&diagnostics](std::string_view value) { diagnostics.emplace_back(value); };

    CHECK(npucompute::CollectDevice0Info(api, &device, &cpu, &aiCore, &memory, &sink));
    CHECK(device.npuCount == 0);
    CHECK(api.SpecializedCallCount() == 0);
    CHECK(Contains(diagnostics, "device count"));

    FakeHardwareDeviceApi nullApi;
    diagnostics.clear();
    CHECK(!npucompute::CollectDevice0Info(nullApi, nullptr, &cpu, &aiCore, &memory, &sink));
    CHECK(nullApi.deviceCountCalls == 0);
    CHECK(Contains(diagnostics, "output is null"));
    return true;
}

bool TestHbmCapacityParsing()
{
    struct Case {
        std::string text;
        bool valid;
        uint64_t bytes;
    };
    const Case cases[] = {
        {"0", true, 0},
        {"0001048576", true, 1048576},
        {" \t1048576\r\n", true, 1048576},
        {"18446744073709551615", true, std::numeric_limits<uint64_t>::max()},
        {"", false, 0},
        {" \t\r\n", false, 0},
        {"18446744073709551616", false, 0},
        {std::string(100, '9'), false, 0},
        {"-1", false, 0},
        {"-0", false, 0},
        {"+1", false, 0},
        {"12x", false, 0},
        {"1 2", false, 0},
        {"1e3", false, 0},
        {std::string("12\0x", 4), false, 0},
    };
    for (const Case& test : cases) {
        FakeHardwareDeviceApi api;
        api.platformValues[ACL_PLATFORM_MEMORY_SIZE] = test.text;
        npucompute::DeviceInfo device;
        npucompute::CpuInfo cpu;
        npucompute::AiCoreInfo aiCore;
        npucompute::MemoryInfo memory;
        memory.hbmTotalMb = 99;
        std::vector<std::string> diagnostics;
        npucompute::DiagnosticSink sink = [&diagnostics](std::string_view value) { diagnostics.emplace_back(value); };
        CHECK(npucompute::CollectDevice0Info(api, &device, &cpu, &aiCore, &memory, &sink));
        CHECK(memory.hbmTotalMb == static_cast<double>(test.bytes) / (1024.0 * 1024.0));
        CHECK(Contains(diagnostics, "invalid HBM total size") == !test.valid);
        CHECK(diagnostics.size() == (test.valid ? 0U : 1U));
        CHECK(memory.hbmUsedMb == 6.0);
    }
    return true;
}

} // namespace

TEST(NpuComputeHardwareInfoDevice, CoreCountsRetainValidSideWhenOtherSideFails)
{
    for (const auto failed : {ACL_DEV_ATTR_CUBE_CORE_NUM, ACL_DEV_ATTR_VECTOR_CORE_NUM}) {
        FakeHardwareDeviceApi api;
        api.failedDeviceAttributes.insert(failed);
        std::uint32_t cubeCount = 99;
        std::uint32_t vectorCount = 99;
        EXPECT_FALSE(npucompute::CollectAiCoreCounts(api, &cubeCount, &vectorCount, nullptr));
        EXPECT_EQ(cubeCount, failed == ACL_DEV_ATTR_CUBE_CORE_NUM ? 0U : 36U);
        EXPECT_EQ(vectorCount, failed == ACL_DEV_ATTR_VECTOR_CORE_NUM ? 0U : 72U);
    }
}

TEST(NpuComputeHardwareInfoDevice, SummaryRetainsIndependentHardwareInputs)
{
    FakeHardwareDeviceApi api;
    api.currentAicFrequency = 1500;
    api.ratedAivFrequency = 1700;
    api.failedDeviceAttributes.insert(ACL_DEV_ATTR_VECTOR_CORE_NUM);
    const auto summary = npucompute::CollectSummaryHardwareInfo(api);
    EXPECT_TRUE(summary.collected);
    EXPECT_EQ(summary.socName, api.socName);
    EXPECT_EQ(summary.cubeCoreCount, 36U);
    EXPECT_FALSE(summary.vectorCoreCount);
    EXPECT_EQ(summary.currentAicFrequencyMhz, 1500);
    EXPECT_FALSE(summary.currentAivFrequencyMhz);
    EXPECT_FALSE(summary.ratedAicFrequencyMhz);
    EXPECT_EQ(summary.ratedAivFrequencyMhz, 1700);
}

TEST(NpuComputeHardwareInfoDevice, SummaryDoesNotInventDefaultHardwareValues)
{
    FakeHardwareDeviceApi api;
    api.failSocName = true;
    api.failedDeviceAttributes = {ACL_DEV_ATTR_CUBE_CORE_NUM, ACL_DEV_ATTR_VECTOR_CORE_NUM};
    const auto summary = npucompute::CollectSummaryHardwareInfo(api);
    EXPECT_TRUE(summary.collected);
    EXPECT_FALSE(summary.socName);
    EXPECT_FALSE(summary.cubeCoreCount);
    EXPECT_FALSE(summary.vectorCoreCount);
    EXPECT_FALSE(summary.currentAicFrequencyMhz);
    EXPECT_FALSE(summary.currentAivFrequencyMhz);
    EXPECT_FALSE(summary.ratedAicFrequencyMhz);
    EXPECT_FALSE(summary.ratedAivFrequencyMhz);
}

static int RunSuiteMain()
{
    if (!TestHbmCapacityParsing() || !TestCompleteMapping() || !TestCollectAiCoreCountsOnlyReadsCountAttributes() ||
        !TestCollectAiCoreFrequencies() || !TestPartialFailuresAndInvalidValues() ||
        !TestNoVisibleDeviceSkipsDeviceQueries() || !TestInvalidDeviceCountAndOutputPointers()) {
        return 1;
    }
    return 0;
}

TEST(NpuComputeHardwareInfoDevice, Main)
{
    ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputeHardwareInfoDevice reported failure";
}
