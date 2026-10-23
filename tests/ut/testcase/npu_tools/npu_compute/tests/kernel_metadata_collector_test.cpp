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

#include "injection/runtime_stub_api.h"
#include "runtime/kernel_metadata_collector.h"
#include <cstring>
#include <array>
#include <iostream>
#include <limits>
#include <string>

static_assert(ACLPTI_RUNTIME_CBID_aclrtMallocWithCfg == 20);
static_assert(ACLPTI_RUNTIME_CBID_aclrtBinaryLoadFromFile == 21);

extern "C" __attribute__((noinline)) void MetadataKernelFixture() {}

namespace {
aclError ResolveEntryKernelName(aclrtFuncHandle, uint32_t bytes, char* name)
{
    constexpr char kName[] = "aclnn_entry_kernel";
    if (name == nullptr || bytes < sizeof(kName)) {
        return ACL_ERROR_INVALID_PARAM;
    }
    std::memcpy(name, kName, sizeof(kName));
    return ACL_SUCCESS;
}

aclError RejectEntryKernelName(aclrtFuncHandle, uint32_t, char*) { return ACL_ERROR_INVALID_PARAM; }

aclError GetDeviceZero(int32_t* deviceId)
{
    *deviceId = 0;
    return ACL_SUCCESS;
}

aclError GetDeviceOne(int32_t* deviceId)
{
    *deviceId = 1;
    return ACL_SUCCESS;
}

aclError RejectDeviceQuery(int32_t* deviceId)
{
    *deviceId = 99;
    return ACL_ERROR_INVALID_PARAM;
}

class NpuComputeKernelDeviceMetadata : public ::testing::Test {
protected:
    void TearDown() override { EXPECT_EQ(RuntimeStubClearOrigin("aclrtGetDevice"), ACL_SUCCESS); }

    void ExpectDevice(std::optional<int32_t> expected)
    {
        aclptiAclrtLaunchKernelParams launch{};
        aclptiAclrtLaunchKernelWithHostArgsParams hostLaunch{};
        aclptiAclrtLaunchKernelWithArgsArrayParams arrayLaunch{};
        aclptiAclrtLaunchSIMTKernelWithHostArgsParams simtHostLaunch{};
        aclptiAclrtLaunchSIMTKernelWithArgsArrayParams simtArrayLaunch{};
        const std::pair<aclptiCallbackId, void*> launches[] = {
            {ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, &launch},
            {ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs, &hostLaunch},
            {ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray, &arrayLaunch},
            {ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs, &simtHostLaunch},
            {ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray, &simtArrayLaunch},
        };
        for (const auto& entry : launches) {
            SCOPED_TRACE(entry.first);
            npucompute::KernelMetadataCollector collector;
            aclptiCallbackData data{};
            data.callbackSite = ACLPTI_API_ENTER;
            data.functionParams = entry.second;
            collector.OnCallback(entry.first, data);
            EXPECT_EQ(collector.Snapshot().deviceId, expected);
        }
    }
};
} // namespace

TEST_F(NpuComputeKernelDeviceMetadata, RecordsCurrentDeviceZero)
{
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetDevice", &GetDeviceZero), ACL_SUCCESS);
    ExpectDevice(0);
}

TEST_F(NpuComputeKernelDeviceMetadata, RecordsCurrentDeviceOne)
{
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetDevice", &GetDeviceOne), ACL_SUCCESS);
    ExpectDevice(1);
}

TEST_F(NpuComputeKernelDeviceMetadata, LeavesDeviceUnavailableWhenQueryFails)
{
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetDevice", &RejectDeviceQuery), ACL_SUCCESS);
    ExpectDevice(std::nullopt);
}

#define CHECK(condition)                                      \
    do {                                                      \
        if (!(condition)) {                                   \
            ADD_FAILURE_AT(__FILE__, __LINE__) << #condition; \
            return 1;                                         \
        }                                                     \
    } while (false)

static int RunSuiteMain()
{
    for (const bool success : {false, true}) {
        npucompute::KernelMetadataCollector bySymbol;
        aclrtFuncHandle symbolHandle = reinterpret_cast<aclrtFuncHandle>(0x2000);
        aclptiAclrtGetFuncBySymbolParams lookup{reinterpret_cast<const void*>(&MetadataKernelFixture), &symbolHandle};
        aclptiCallbackData callback{};
        callback.functionParams = &lookup;
        callback.callbackSite = ACLPTI_API_ENTER;
        bySymbol.OnCallback(ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol, callback);
        lookup.symbol = nullptr;
        callback.callbackSite = ACLPTI_API_EXIT;
        callback.retval = success ? ACL_SUCCESS : 1;
        bySymbol.OnCallback(ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol, callback);
        aclptiAclrtLaunchKernelWithArgsArrayParams launchArgs{};
        launchArgs.func = symbolHandle;
        launchArgs.numBlocks = 8;
        callback.functionParams = &launchArgs;
        callback.callbackSite = ACLPTI_API_ENTER;
        bySymbol.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray, callback);
        const auto captured = bySymbol.Snapshot();
        CHECK(success ? captured.name == "MetadataKernelFixture" : !captured.name);
        CHECK(captured.blockDim == 8);
    }
    for (const void* symbol : {static_cast<const void*>(nullptr), reinterpret_cast<const void*>(0x1234)}) {
        npucompute::KernelMetadataCollector unknown;
        aclrtFuncHandle unknownHandle = reinterpret_cast<aclrtFuncHandle>(0x3000);
        aclptiAclrtGetFuncBySymbolParams lookup{symbol, &unknownHandle};
        aclptiCallbackData callback{};
        callback.functionParams = &lookup;
        callback.callbackSite = ACLPTI_API_ENTER;
        unknown.OnCallback(ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol, callback);
        callback.callbackSite = ACLPTI_API_EXIT;
        callback.retval = ACL_SUCCESS;
        unknown.OnCallback(ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol, callback);
        aclptiAclrtLaunchKernelWithArgsArrayParams args{};
        args.func = unknownHandle;
        callback.functionParams = &args;
        callback.callbackSite = ACLPTI_API_ENTER;
        unknown.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray, callback);
        CHECK(!unknown.Snapshot().name);
    }
    npucompute::KernelMetadataCollector collector;
    aclrtFuncHandle handle = nullptr;
    std::string name = "original kernel";
    aclptiAclrtBinaryGetFunctionParams params{nullptr, name.c_str(), &handle};
    aclptiCallbackData data{};
    data.functionParams = &params;
    data.callbackSite = ACLPTI_API_ENTER;
    collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction, data);
    name = "changed";
    params.kernelName = nullptr;
    handle = reinterpret_cast<aclrtFuncHandle>(0x1000);
    data.callbackSite = ACLPTI_API_EXIT;
    data.retval = ACL_SUCCESS;
    collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction, data);
    aclptiAclrtLaunchKernelParams launch{};
    launch.funcHandle = handle;
    launch.numBlocks = 64;
    data.functionParams = &launch;
    data.callbackSite = ACLPTI_API_ENTER;
    collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, data);
    auto metadata = collector.Snapshot();
    CHECK(metadata.name == "original kernel");
    CHECK(metadata.blockDim == 64);
    launch.numBlocks = 128;
    collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, data);
    CHECK(collector.Snapshot().blockDim == 64);

    npucompute::KernelMetadataCollector failed;
    params.kernelName = "failed";
    data.functionParams = &params;
    failed.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction, data);
    data.callbackSite = ACLPTI_API_EXIT;
    data.retval = 1;
    failed.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction, data);
    data.functionParams = &launch;
    data.callbackSite = ACLPTI_API_ENTER;
    failed.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, data);
    CHECK(!failed.Snapshot().name);

    npucompute::KernelMetadataCollector byEntry;
    CHECK(RuntimeStubSetOriginFunction("aclrtGetFunctionName", &ResolveEntryKernelName) == ACL_SUCCESS);
    aclrtFuncHandle entryHandle = nullptr;
    aclptiAclrtBinaryGetFunctionByEntryParams entryParams{nullptr, 42, &entryHandle};
    data.functionParams = &entryParams;
    data.callbackSite = ACLPTI_API_ENTER;
    byEntry.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry, data);
    entryHandle = reinterpret_cast<aclrtFuncHandle>(0x4000);
    data.callbackSite = ACLPTI_API_EXIT;
    data.retval = ACL_SUCCESS;
    byEntry.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry, data);
    aclptiAclrtLaunchKernelParams entryLaunch{};
    entryLaunch.funcHandle = entryHandle;
    entryLaunch.numBlocks = 16;
    data.functionParams = &entryLaunch;
    data.callbackSite = ACLPTI_API_ENTER;
    byEntry.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, data);
    const auto entryMetadata = byEntry.Snapshot();
    CHECK(entryMetadata.name == "aclnn_entry_kernel");
    CHECK(entryMetadata.blockDim == 16);

    npucompute::KernelMetadataCollector unresolvedEntry;
    CHECK(RuntimeStubSetOriginFunction("aclrtGetFunctionName", &RejectEntryKernelName) == ACL_SUCCESS);
    entryHandle = nullptr;
    entryParams.funcEntry = 43;
    data.functionParams = &entryParams;
    data.callbackSite = ACLPTI_API_ENTER;
    unresolvedEntry.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry, data);
    entryHandle = reinterpret_cast<aclrtFuncHandle>(0x4001);
    data.callbackSite = ACLPTI_API_EXIT;
    data.retval = ACL_SUCCESS;
    unresolvedEntry.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry, data);
    entryLaunch.funcHandle = entryHandle;
    data.functionParams = &entryLaunch;
    data.callbackSite = ACLPTI_API_ENTER;
    unresolvedEntry.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, data);
    CHECK(unresolvedEntry.Snapshot().name == "entry_43");

    for (bool overflow : {false, true}) {
        npucompute::KernelMetadataCollector simt;
        aclptiAclrtLaunchSIMTKernelWithArgsArrayParams simtLaunch{};
        simtLaunch.gridDim.x = overflow ? std::numeric_limits<uint32_t>::max() : 2;
        simtLaunch.gridDim.y = overflow ? std::numeric_limits<uint32_t>::max() : 3;
        simtLaunch.gridDim.z = overflow ? std::numeric_limits<uint32_t>::max() : 4;
        data.functionParams = &simtLaunch;
        simt.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray, data);
        CHECK(overflow ? !simt.Snapshot().blockDim : simt.Snapshot().blockDim == 24);
    }
    for (const bool hostArgs : {false, true}) {
        npucompute::KernelMetadataCollector simt;
        params.kernelName = "simt kernel";
        data.functionParams = &params;
        data.callbackSite = ACLPTI_API_ENTER;
        simt.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction, data);
        data.callbackSite = ACLPTI_API_EXIT;
        data.retval = ACL_SUCCESS;
        simt.OnCallback(ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction, data);
        aclptiAclrtLaunchSIMTKernelWithArgsArrayParams arrayLaunch{};
        arrayLaunch.func = handle;
        arrayLaunch.gridDim = {2, 3, 4};
        aclptiAclrtLaunchSIMTKernelWithHostArgsParams hostLaunch{};
        hostLaunch.func = handle;
        hostLaunch.gridDim = {2, 3, 4};
        data.functionParams = hostArgs ? static_cast<void*>(&hostLaunch) : static_cast<void*>(&arrayLaunch);
        data.callbackSite = ACLPTI_API_ENTER;
        simt.OnCallback(
            hostArgs ? ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs :
                       ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray,
            data);
        CHECK(simt.Snapshot().name == "simt kernel");
        CHECK(simt.Snapshot().blockDim == 24);
    }
    return 0;
}

TEST(NpuComputeKernelMetadataCollector, Main)
{
    ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputeKernelMetadataCollector reported failure";
}

namespace {
int64_t g_metadataKernelType = ACL_KERNEL_TYPE_MIX;
bool g_metadataQueryFailure = false;
uint32_t g_rawKernelType = 4;             // Runtime kernel metadata: MIX_AIC_MAIN.
std::array<uint16_t, 2> g_rawRatio{1, 2}; // Metadata order is AIC then AIV.
bool g_rawQueryFailure = true;
bool g_rawTypeQueryFailure = false;
uint32_t g_rawRatioQueryCount = 0;
std::string g_metadataSoc = "Ascend950PR_9599";

aclError QueryKernelAttribute(const void*, aclrtFuncAttribute attr, int64_t* value)
{
    EXPECT_EQ(attr, ACL_FUNC_ATTR_KERNEL_TYPE); // 950 ratio must come from raw metadata.
    if (g_metadataQueryFailure || attr != ACL_FUNC_ATTR_KERNEL_TYPE) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *value = g_metadataKernelType;
    return ACL_SUCCESS;
}

const char* QueryMetadataSoc() { return g_metadataSoc.c_str(); }

class MetadataInputTest : public testing::Test {
protected:
    void SetUp() override
    {
        g_metadataKernelType = ACL_KERNEL_TYPE_MIX;
        g_metadataQueryFailure = false;
        g_rawQueryFailure = false;
        g_rawTypeQueryFailure = false;
        g_rawRatioQueryCount = 0;
        g_rawKernelType = 4;
        g_rawRatio = {1, 2};
        g_metadataSoc = "Ascend950PR_9599";
        ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetFunctionAttribute", &QueryKernelAttribute), ACL_SUCCESS);
        ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetSocName", &QueryMetadataSoc), ACL_SUCCESS);
    }

    void TearDown() override
    {
        EXPECT_EQ(RuntimeStubClearOrigin("aclrtGetFunctionAttribute"), ACL_SUCCESS);
        EXPECT_EQ(RuntimeStubClearOrigin("aclrtGetSocName"), ACL_SUCCESS);
        g_rawQueryFailure = true;
    }

    npucompute::KernelMetadata Capture(uint32_t blocks = 8)
    {
        npucompute::KernelMetadataCollector collector;
        Launch(collector, blocks);
        return collector.Snapshot();
    }

    void Launch(npucompute::KernelMetadataCollector& collector, uint32_t blocks)
    {
        launch_.funcHandle = reinterpret_cast<aclrtFuncHandle>(0x1000);
        launch_.numBlocks = blocks;
        callback_.functionParams = &launch_;
        callback_.callbackSite = ACLPTI_API_ENTER;
        collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, callback_);
    }

    aclptiAclrtLaunchKernelParams launch_{};
    aclptiCallbackData callback_{};
};
} // namespace

// Real runtime exports are discovered dynamically; these fixtures exercise the same query boundary.
extern "C" int32_t rtFunctionGetMetaInfoSize(const void*, uint32_t type, size_t* size)
{
    if (type == 3) {
        ++g_rawRatioQueryCount;
    }
    if (g_rawQueryFailure || (type == 1 && g_rawTypeQueryFailure) || (type != 1 && type != 3)) {
        return 1;
    }
    *size = sizeof(uint32_t);
    return 0;
}

extern "C" int32_t rtFunctionGetMetaInfo(const void*, uint32_t type, void* data, uint32_t size)
{
    if (g_rawQueryFailure || (type == 1 && g_rawTypeQueryFailure) || size != sizeof(uint32_t)) {
        return 1;
    }
    if (type == 1) {
        std::memcpy(data, &g_rawKernelType, size);
    } else if (type == 3) {
        std::memcpy(data, g_rawRatio.data(), size);
    } else {
        return 1;
    }
    return 0;
}

TEST_F(MetadataInputTest, Decodes950MetadataRatio)
{
    g_rawRatio = {1, 0};
    auto metadata = Capture();
    EXPECT_EQ(metadata.kind, npucompute::KernelKind::Cube);
    EXPECT_EQ(metadata.blockDim, 8U);
    EXPECT_FALSE(metadata.mixBlockDim);
    g_rawRatio = {0, 1};
    metadata = Capture();
    EXPECT_EQ(metadata.kind, npucompute::KernelKind::Vector);
    EXPECT_EQ(metadata.blockDim, 8U);
    EXPECT_FALSE(metadata.mixBlockDim);
    for (const uint16_t vectors : {1, 2}) {
        g_rawRatio = {1, vectors};
        const auto metadata = Capture();
        EXPECT_EQ(metadata.kind, npucompute::KernelKind::Mix);
        EXPECT_EQ(metadata.mixBlockDim, 8U * vectors);
    }
    EXPECT_EQ(Capture(7).mixBlockDim, 14U);
    EXPECT_FALSE(Capture(0).mixBlockDim);
}

TEST_F(MetadataInputTest, QueriesOriginalAttributeWhenPublicDispatchIsReplaced)
{
    aclrtApiFunc origin = nullptr;
    aclrtApiFunc current = nullptr;
    ASSERT_EQ(aclrtApiInjectionGetFunc("aclrtGetFunctionAttribute", &origin, &current), ACL_SUCCESS);
    const auto reject = +[](const void*, aclrtFuncAttribute, int64_t*) -> aclError { return ACL_ERROR_INVALID_PARAM; };
    ASSERT_EQ(
        aclrtApiInjectionSetFunc("aclrtGetFunctionAttribute", reinterpret_cast<aclrtApiFunc>(reject)), ACL_SUCCESS);
    const auto metadata = Capture();
    EXPECT_EQ(aclrtApiInjectionSetFunc("aclrtGetFunctionAttribute", current), ACL_SUCCESS);
    EXPECT_EQ(metadata.kind, npucompute::KernelKind::Mix);
    EXPECT_EQ(metadata.mixBlockDim, 16U);
}

TEST_F(MetadataInputTest, UsesMixRatioWithoutMainSideGuard)
{
    g_metadataQueryFailure = true;
    g_rawTypeQueryFailure = true;
    EXPECT_EQ(Capture().kind, npucompute::KernelKind::Mix);
    EXPECT_EQ(Capture().mixBlockDim, 16U);
    g_metadataQueryFailure = false;
    g_rawTypeQueryFailure = false;
    g_rawKernelType = 5; // MIX_AIV_MAIN
    EXPECT_EQ(Capture().kind, npucompute::KernelKind::Mix);
    EXPECT_EQ(Capture().mixBlockDim, 16U);
}

TEST_F(MetadataInputTest, DoesNotInferMixCountFromInvalidOrMissingRatio)
{
    for (const std::array<uint16_t, 2> ratio : {std::array<uint16_t, 2>{0, 0}, {0, 2}, {1, 3}, {2, 3}}) {
        g_rawRatio = ratio;
        EXPECT_FALSE(Capture().mixBlockDim);
    }
    g_rawQueryFailure = true;
    EXPECT_EQ(Capture().kind, npucompute::KernelKind::Mix);
    EXPECT_FALSE(Capture().mixBlockDim);
    g_metadataQueryFailure = true;
    EXPECT_FALSE(Capture().kind);
}

TEST_F(MetadataInputTest, PreservesPureKernelTypeWithoutPmu)
{
    g_rawQueryFailure = true;
    g_metadataKernelType = ACL_KERNEL_TYPE_CUBE;
    EXPECT_EQ(Capture().kind, npucompute::KernelKind::Cube);
    EXPECT_FALSE(Capture().mixBlockDim);
    g_metadataKernelType = ACL_KERNEL_TYPE_VECTOR;
    EXPECT_EQ(Capture().kind, npucompute::KernelKind::Vector);
    g_metadataKernelType = ACL_KERNEL_TYPE_AICORE;
    EXPECT_FALSE(Capture().kind); // The legacy generic type is not proof of MIX.
    g_metadataKernelType = ACL_KERNEL_TYPE_AICPU;
    EXPECT_FALSE(Capture().kind);
}

TEST_F(MetadataInputTest, DoesNotQueryMixRatioForKnownPureKernel)
{
    for (const uint32_t type : {2U, 3U, 6U, 7U}) {
        g_rawKernelType = type;
        const bool vector = type == 3U || type == 7U;
        g_metadataKernelType = vector ? ACL_KERNEL_TYPE_VECTOR : ACL_KERNEL_TYPE_CUBE;
        const auto metadata = Capture();
        EXPECT_EQ(metadata.kind, vector ? npucompute::KernelKind::Vector : npucompute::KernelKind::Cube);
        EXPECT_FALSE(metadata.mixBlockDim);
        EXPECT_EQ(g_rawRatioQueryCount, 0U);
    }
}

TEST_F(MetadataInputTest, StillQueriesMixRatioForVectorMainMixedKernel)
{
    constexpr uint32_t kMixVectorMainType = 5;
    g_rawKernelType = kMixVectorMainType;
    g_metadataKernelType = ACL_KERNEL_TYPE_VECTOR;
    const auto metadata = Capture();
    EXPECT_EQ(metadata.kind, npucompute::KernelKind::Mix);
    EXPECT_EQ(metadata.mixBlockDim, 16U);
    EXPECT_EQ(g_rawRatioQueryCount, 1U);
}

TEST_F(MetadataInputTest, UsesKernelMetadataRatioOn950)
{
    for (const char* soc : {"Ascend950PR_9599", "Ascend950DT_9591"}) {
        g_metadataSoc = soc;
        for (const uint16_t vectors : {1, 2}) {
            g_rawRatio = {1, vectors};
            EXPECT_EQ(Capture().mixBlockDim, 8U * vectors);
        }
    }
    g_rawRatio = {0, 2};
    EXPECT_FALSE(Capture().mixBlockDim);
}

TEST_F(MetadataInputTest, RejectsMixDimensionOnUnsupportedOrUnknownProduct)
{
    for (const auto* soc : {"Ascend910B1", "Ascend910_9391", "unknown", ""}) {
        g_metadataSoc = soc;
        EXPECT_EQ(Capture().kind, npucompute::KernelKind::Mix);
        EXPECT_FALSE(Capture().mixBlockDim);
    }
}

TEST_F(MetadataInputTest, DiscardsFailedLaunchBeforeChoosingSuccessfulTarget)
{
    npucompute::KernelMetadataCollector collector;
    Launch(collector, 8);
    callback_.callbackSite = ACLPTI_API_EXIT;
    callback_.retval = ACL_ERROR_INVALID_PARAM;
    collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, callback_);
    const auto failed = collector.Snapshot();
    EXPECT_FALSE(failed.blockDim);
    EXPECT_FALSE(failed.kind);
}

TEST_F(MetadataInputTest, FreezesTypeAndDimensionsForOriginalTarget)
{
    npucompute::KernelMetadataCollector collector;
    Launch(collector, 8);
    callback_.callbackSite = ACLPTI_API_EXIT;
    callback_.retval = ACL_SUCCESS;
    collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, callback_);
    g_metadataKernelType = ACL_KERNEL_TYPE_VECTOR;
    Launch(collector, 32);
    const auto metadata = collector.Snapshot();
    EXPECT_EQ(metadata.kind, npucompute::KernelKind::Mix);
    EXPECT_EQ(metadata.blockDim, 8U);
    EXPECT_EQ(metadata.mixBlockDim, 16U);
}

TEST_F(MetadataInputTest, RejectsMixDimensionOverflow)
{
    npucompute::KernelMetadataCollector collector;
    aclptiAclrtLaunchSIMTKernelWithArgsArrayParams launch{};
    launch.func = reinterpret_cast<void*>(0x1000);
    launch.gridDim = {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(), 1};
    callback_.functionParams = &launch;
    callback_.callbackSite = ACLPTI_API_ENTER;
    collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray, callback_);
    const auto metadata = collector.Snapshot();
    EXPECT_TRUE(metadata.blockDim);
    EXPECT_EQ(metadata.kind, npucompute::KernelKind::Mix);
    EXPECT_FALSE(metadata.mixBlockDim);
}

TEST_F(MetadataInputTest, ChoosesSuccessfulLaunchAfterFailure)
{
    npucompute::KernelMetadataCollector collector;
    Launch(collector, 8);
    callback_.callbackSite = ACLPTI_API_EXIT;
    callback_.retval = ACL_ERROR_INVALID_PARAM;
    collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, callback_);
    Launch(collector, 16);
    callback_.callbackSite = ACLPTI_API_EXIT;
    callback_.retval = ACL_SUCCESS;
    collector.OnCallback(ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, callback_);
    const auto metadata = collector.Snapshot();
    EXPECT_EQ(metadata.blockDim, 16U);
    EXPECT_EQ(metadata.mixBlockDim, 32U);
}
