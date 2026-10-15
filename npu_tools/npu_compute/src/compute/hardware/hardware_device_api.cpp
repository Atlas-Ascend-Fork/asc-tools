/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "hardware/hardware_device_api.h"

#include <acl/acl.h>
#include <acl/acl_platform.h>
#include <driver/ascend_hal_base.h>
#include <driver/dsmi_common_interface.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>

#include <dlfcn.h>

namespace npucompute {
namespace {

constexpr char kAclLibrary[] = "libascendcl.so";
constexpr char kPlatformLibrary[] = "libplatform.so";
constexpr char kHalLibrary[] = "libascend_hal.so";
constexpr char kDsmiLibrary[] = "libdrvdsmi_host.so";
constexpr std::size_t kPlatformValueSize = 4096;

class PosixDynamicSymbolResolver final : public DynamicSymbolResolver {
public:
    void* FindLoadedSymbol(const char* name) override { return ::dlsym(RTLD_DEFAULT, name); }

    void* OpenLibrary(const char* soname) override { return ::dlopen(soname, RTLD_NOW | RTLD_LOCAL); }

    void* FindLibrarySymbol(void* handle, const char* name) override
    {
        return handle == nullptr ? nullptr : ::dlsym(handle, name);
    }

    void CloseLibrary(void* handle) override
    {
        if (handle != nullptr) {
            ::dlclose(handle);
        }
    }
};

template <typename Function>
Function ToFunction(void* symbol)
{
    static_assert(std::is_pointer_v<Function>);
    static_assert(sizeof(Function) == sizeof(symbol));
    Function function = nullptr;
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

template <typename Character, std::size_t Size>
std::string BoundedString(const Character (&value)[Size])
{
    std::size_t length = 0;
    while (length < Size && value[length] != 0) {
        ++length;
    }
    return std::string(reinterpret_cast<const char*>(value), length);
}

} // namespace

class DynamicHardwareDeviceApi::Impl {
public:
    explicit Impl(std::shared_ptr<DynamicSymbolResolver> resolver) : resolver_(std::move(resolver))
    {
        if (resolver_ == nullptr) {
            resolver_ = std::make_shared<PosixDynamicSymbolResolver>();
        }
    }

    ~Impl()
    {
        for (const auto& library : libraries_) {
            resolver_->CloseLibrary(library.second);
        }
    }

    void* Resolve(const char* name, const char* fallbackLibrary)
    {
        if (void* symbol = resolver_->FindLoadedSymbol(name); symbol != nullptr) {
            return symbol;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        void* handle = GetLibrary(fallbackLibrary);
        return handle == nullptr ? nullptr : resolver_->FindLibrarySymbol(handle, name);
    }

private:
    void* GetLibrary(const char* soname)
    {
        const auto library = libraries_.find(soname);
        if (library != libraries_.end()) {
            return library->second;
        }
        if (attemptedLibraries_.count(soname) != 0) {
            return nullptr;
        }

        attemptedLibraries_.insert(soname);
        void* handle = resolver_->OpenLibrary(soname);
        if (handle != nullptr) {
            libraries_.emplace(soname, handle);
        }
        return handle;
    }

    std::shared_ptr<DynamicSymbolResolver> resolver_;
    std::mutex mutex_;
    std::map<std::string, void*> libraries_;
    std::set<std::string> attemptedLibraries_;
};

DynamicHardwareDeviceApi::DynamicHardwareDeviceApi()
    : impl_(std::make_unique<Impl>(std::make_shared<PosixDynamicSymbolResolver>()))
{}

DynamicHardwareDeviceApi::DynamicHardwareDeviceApi(std::shared_ptr<DynamicSymbolResolver> resolver)
    : impl_(std::make_unique<Impl>(std::move(resolver)))
{}

DynamicHardwareDeviceApi::~DynamicHardwareDeviceApi() = default;

bool DynamicHardwareDeviceApi::GetDeviceCount(std::int32_t* value)
{
    if (value == nullptr) {
        return false;
    }
    using Function = aclError (*)(uint32_t*);
    const Function function = ToFunction<Function>(impl_->Resolve("aclrtGetDeviceCount", kAclLibrary));
    if (function == nullptr) {
        return false;
    }

    uint32_t count = 0;
    if (function(&count) != ACL_SUCCESS || count > static_cast<uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    *value = static_cast<std::int32_t>(count);
    return true;
}

bool DynamicHardwareDeviceApi::GetSocName(std::string* value)
{
    if (value == nullptr) {
        return false;
    }
    using Function = const char* (*)();
    const Function function = ToFunction<Function>(impl_->Resolve("aclrtGetSocName", kAclLibrary));
    if (function == nullptr) {
        return false;
    }
    const char* socName = function();
    if (socName == nullptr) {
        return false;
    }
    *value = socName;
    return true;
}

bool DynamicHardwareDeviceApi::GetDeviceAttribute(std::int32_t deviceId, std::int32_t attribute, std::int64_t* value)
{
    if (deviceId < 0 || value == nullptr) {
        return false;
    }
    using Function = aclError (*)(uint32_t, aclrtDevAttr, std::int64_t*);
    const Function function = ToFunction<Function>(impl_->Resolve("aclrtGetDeviceInfo", kAclLibrary));
    return function != nullptr &&
           function(static_cast<uint32_t>(deviceId), static_cast<aclrtDevAttr>(attribute), value) == ACL_SUCCESS;
}

bool DynamicHardwareDeviceApi::GetPlatformValue(std::int32_t type, std::string* value)
{
    if (value == nullptr) {
        return false;
    }
    using Function = aclError (*)(aclplatformDevInfo, char*, uint32_t);
    const Function function = ToFunction<Function>(impl_->Resolve("aclplatformGetDeviceInfo", kPlatformLibrary));
    if (function == nullptr) {
        return false;
    }

    std::array<char, kPlatformValueSize> buffer{};
    if (function(static_cast<aclplatformDevInfo>(type), buffer.data(), static_cast<uint32_t>(buffer.size())) !=
        ACL_SUCCESS) {
        return false;
    }
    const auto terminator = std::find(buffer.begin(), buffer.end(), static_cast<char>(0));
    if (terminator == buffer.end()) {
        return false;
    }
    value->assign(buffer.begin(), terminator);
    return true;
}

bool DynamicHardwareDeviceApi::GetControlCpuCount(std::int32_t deviceId, uint32_t* value)
{
    if (deviceId < 0 || value == nullptr) {
        return false;
    }
    using Function = drvError_t (*)(uint32_t, std::int32_t, std::int32_t, std::int64_t*);
    const Function function = ToFunction<Function>(impl_->Resolve("halGetDeviceInfo", kHalLibrary));
    if (function == nullptr) {
        return false;
    }

    std::int64_t count = 0;
    if (function(static_cast<uint32_t>(deviceId), MODULE_TYPE_CCPU, INFO_TYPE_CORE_NUM, &count) != 0 || count < 0 ||
        static_cast<uint64_t>(count) > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    *value = static_cast<uint32_t>(count);
    return true;
}

bool DynamicHardwareDeviceApi::GetAiCpuFrequency(std::int32_t deviceId, uint32_t* value)
{
    if (deviceId < 0 || value == nullptr) {
        return false;
    }
    using Function = int (*)(int, struct dsmi_aicpu_info_stru*);
    const Function function = ToFunction<Function>(impl_->Resolve("dsmi_get_aicpu_info", kDsmiLibrary));
    if (function == nullptr) {
        return false;
    }

    struct dsmi_aicpu_info_stru information {};
    if (function(deviceId, &information) != 0) {
        return false;
    }
    *value = information.curFreq;
    return true;
}

bool DynamicHardwareDeviceApi::GetAiCoreFrequencies(
    std::int32_t deviceId, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz)
{
    return ReadAiCoreFrequencies(deviceId, aicFrequencyMhz, aivFrequencyMhz, true);
}

bool DynamicHardwareDeviceApi::GetRatedAiCoreFrequencies(
    std::int32_t deviceId, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz)
{
    return ReadAiCoreFrequencies(deviceId, aicFrequencyMhz, aivFrequencyMhz, false);
}

bool DynamicHardwareDeviceApi::GetCurrentAiCoreFrequencies(
    std::int32_t deviceId, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz)
{
    return ReadAiCoreFrequencies(deviceId, aicFrequencyMhz, aivFrequencyMhz, true, false);
}

bool DynamicHardwareDeviceApi::ReadAiCoreFrequencies(
    std::int32_t deviceId, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz, bool currentFirst,
    bool allowRatedFallback)
{
    if (aicFrequencyMhz == nullptr || aivFrequencyMhz == nullptr) {
        return false;
    }
    *aicFrequencyMhz = 0;
    *aivFrequencyMhz = 0;
    if (deviceId < 0) {
        return false;
    }

    using Function = drvError_t (*)(uint32_t, std::int32_t, std::int32_t, std::int64_t*);
    const Function function = ToFunction<Function>(impl_->Resolve("halGetDeviceInfo", kHalLibrary));
    if (function == nullptr) {
        return false;
    }

    const auto readFrequency = [function, deviceId, currentFirst, allowRatedFallback](
                                   std::int32_t moduleType, uint32_t* value) {
        std::int64_t frequency = 0;
        if (currentFirst &&
            function(static_cast<uint32_t>(deviceId), moduleType, INFO_TYPE_CURRENT_FREQ, &frequency) ==
                DRV_ERROR_NONE &&
            frequency > 0 && static_cast<uint64_t>(frequency) <= std::numeric_limits<uint32_t>::max()) {
            *value = static_cast<uint32_t>(frequency);
            return true;
        }
        if (!allowRatedFallback) {
            return false;
        }
        frequency = 0;
        if (function(static_cast<uint32_t>(deviceId), moduleType, INFO_TYPE_FREQUE, &frequency) != DRV_ERROR_NONE ||
            frequency <= 0 || static_cast<uint64_t>(frequency) > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        *value = static_cast<uint32_t>(frequency);
        return true;
    };

    const bool readAic = readFrequency(MODULE_TYPE_AICORE, aicFrequencyMhz);
    const bool readAiv = readFrequency(MODULE_TYPE_VECTOR_CORE, aivFrequencyMhz);
    return readAic && readAiv;
}

bool DynamicHardwareDeviceApi::GetSyscntFrequencyHz(std::int32_t deviceId, double* frequencyHz)
{
    if (frequencyHz == nullptr) {
        return false;
    }
    *frequencyHz = 0.0;
    if (deviceId < 0) {
        return false;
    }
    using Function = drvError_t (*)(uint32_t, std::int32_t, std::int32_t, std::int64_t*);
    const Function function = ToFunction<Function>(impl_->Resolve("halGetDeviceInfo", kHalLibrary));
    std::int64_t frequencyKhz = 0;
    if (function == nullptr ||
        function(static_cast<uint32_t>(deviceId), MODULE_TYPE_SYSTEM, INFO_TYPE_DEV_OSC_FREQUE, &frequencyKhz) !=
            DRV_ERROR_NONE ||
        frequencyKhz <= 0) {
        return false;
    }
    *frequencyHz = static_cast<double>(frequencyKhz) * 1000.0;
    return true;
}

bool DynamicHardwareDeviceApi::GetChipVersion(std::int32_t deviceId, std::string* value)
{
    if (deviceId < 0 || value == nullptr) {
        return false;
    }
    using Function = int (*)(int, struct dsmi_chip_info_stru*);
    const Function function = ToFunction<Function>(impl_->Resolve("dsmi_get_chip_info", kDsmiLibrary));
    if (function == nullptr) {
        return false;
    }

    struct dsmi_chip_info_stru information {};
    if (function(deviceId, &information) != 0) {
        return false;
    }
    *value = BoundedString(information.chip_ver);
    return true;
}

bool DynamicHardwareDeviceApi::GetHbmUsage(std::int32_t deviceId, uint64_t* freeBytes, uint64_t* totalBytes)
{
    if (deviceId < 0 || freeBytes == nullptr || totalBytes == nullptr) {
        return false;
    }
    using SetDeviceFunction = aclError (*)(std::int32_t);
    using GetMemInfoFunction = aclError (*)(aclrtMemAttr, std::size_t*, std::size_t*);
    const SetDeviceFunction setDevice = ToFunction<SetDeviceFunction>(impl_->Resolve("aclrtSetDevice", kAclLibrary));
    const GetMemInfoFunction getMemInfo =
        ToFunction<GetMemInfoFunction>(impl_->Resolve("aclrtGetMemInfo", kAclLibrary));
    if (setDevice == nullptr || getMemInfo == nullptr || setDevice(deviceId) != ACL_SUCCESS) {
        return false;
    }

    std::size_t freeValue = 0;
    std::size_t totalValue = 0;
    if (getMemInfo(ACL_HBM_MEM, &freeValue, &totalValue) != ACL_SUCCESS) {
        return false;
    }
    *freeBytes = static_cast<uint64_t>(freeValue);
    *totalBytes = static_cast<uint64_t>(totalValue);
    return true;
}

bool DynamicHardwareDeviceApi::GetHbmFrequency(std::int32_t deviceId, uint32_t* value)
{
    if (deviceId < 0 || value == nullptr) {
        return false;
    }
    using Function = int (*)(int, int, unsigned int*);
    const Function function = ToFunction<Function>(impl_->Resolve("dsmi_get_device_frequency", kDsmiLibrary));
    return function != nullptr && function(deviceId, DSMI_DEVICE_TYPE_HBM, value) == 0;
}

} // namespace npucompute
