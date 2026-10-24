/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute/acl_pti_callback_stub.h"

#include <acl/acl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <string>
#include <type_traits>

#include <dlfcn.h>
#include <unistd.h>

namespace {

constexpr int32_t DEVICE_ID = 0;
constexpr char HARDWARE_INFO_FILE[] = "HardwareInfo.jsonl";
constexpr char SECTIONS[] = "PipeUtilization";

class AclRuntimeGuard {
public:
    ~AclRuntimeGuard()
    {
        if (automaticCleanup_) {
            Cleanup();
        }
    }

    void MarkInitialized() noexcept { initialized_ = true; }
    void MarkDeviceSet() noexcept { deviceSet_ = true; }
    void DisableAutomaticCleanup() noexcept { automaticCleanup_ = false; }

    bool Cleanup() noexcept
    {
        bool success = true;
        if (deviceSet_) {
            const aclError result = aclrtResetDevice(DEVICE_ID);
            if (result != ACL_SUCCESS) {
                std::fprintf(
                    stderr, "[real_hardware_callback_app] aclrtResetDevice failed: %d\n", static_cast<int>(result));
                success = false;
            }
            deviceSet_ = false;
        }
        if (initialized_) {
            const aclError result = aclFinalize();
            if (result != ACL_SUCCESS) {
                std::fprintf(stderr, "[real_hardware_callback_app] aclFinalize failed: %d\n", static_cast<int>(result));
                success = false;
            }
            initialized_ = false;
        }
        return success;
    }

private:
    bool initialized_ = false;
    bool deviceSet_ = false;
    bool automaticCleanup_ = true;
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

bool PrepareEnvironment(const boost::filesystem::path& outputDirectory)
{
    if (!outputDirectory.is_absolute()) {
        std::fprintf(stderr, "[real_hardware_callback_app] output directory must be absolute\n");
        return false;
    }

    boost::system::error_code error;
    if (!boost::filesystem::is_directory(outputDirectory, error) || error) {
        std::fprintf(stderr, "[real_hardware_callback_app] output directory is unavailable\n");
        return false;
    }
    if (::access(outputDirectory.c_str(), W_OK | X_OK) != 0) {
        std::perror("[real_hardware_callback_app] output directory is not writable");
        return false;
    }
    return true;
}

bool EmitSuccessfulExit(aclptiCallbackId cbid)
{
    return AclPtiCallbackStubEmitRuntimeEvent(
               static_cast<uint32_t>(cbid), static_cast<uint32_t>(ACLPTI_API_EXIT),
               static_cast<int32_t>(ACL_SUCCESS)) == 1;
}

bool ParseCallbackId(const char* argument, aclptiCallbackId* cbid)
{
    if (std::strcmp(argument, "13") == 0) {
        *cbid = ACLPTI_RUNTIME_CBID_aclrtLaunchKernel;
        return true;
    }
    if (std::strcmp(argument, "0") == 0) {
        *cbid = ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs;
        return true;
    }
    if (std::strcmp(argument, "16") == 0) {
        *cbid = ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs;
        return true;
    }
    if (std::strcmp(argument, "17") == 0) {
        *cbid = ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray;
        return true;
    }
    return false;
}

bool HardwareInfoIsComplete(const boost::filesystem::path& outputDirectory)
{
    const boost::filesystem::path output = outputDirectory / HARDWARE_INFO_FILE;
    boost::system::error_code error;
    if (!boost::filesystem::is_regular_file(output, error) || error || boost::filesystem::is_symlink(output, error) ||
        error) {
        return false;
    }

    std::ifstream stream(output.string());
    if (!stream) {
        return false;
    }
    std::size_t lineCount = 0;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            return false;
        }
        ++lineCount;
    }
    return !stream.bad() && lineCount == 5;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(
            stderr, "usage: %s <test-libnpu_compute_processor.so> <output-directory> <callback-id: 13|0|16|17>\n",
            argv[0]);
        return 2;
    }

    const boost::filesystem::path injectionLibrary = boost::filesystem::absolute(argv[1]);
    const boost::filesystem::path outputDirectory = argv[2];
    aclptiCallbackId callbackId = ACLPTI_RUNTIME_CBID_SIZE;
    if (!ParseCallbackId(argv[3], &callbackId)) {
        std::fprintf(stderr, "[real_hardware_callback_app] unsupported callback ID: %s\n", argv[3]);
        return 3;
    }
    if (!PrepareEnvironment(outputDirectory)) {
        return 4;
    }

    AclRuntimeGuard aclRuntime;
    aclError result = aclInit(nullptr);
    if (result != ACL_SUCCESS) {
        std::fprintf(stderr, "[real_hardware_callback_app] aclInit failed: %d\n", static_cast<int>(result));
        return 5;
    }
    aclRuntime.MarkInitialized();

    void* injectionHandle = ::dlopen(injectionLibrary.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (injectionHandle == nullptr) {
        std::fprintf(stderr, "[real_hardware_callback_app] dlopen failed: %s\n", ::dlerror());
        return 6;
    }
    // acltoolInitialize registers an atexit handler in this library.
    // Keep the handle loaded until process exit so that handler remains valid.

    ::dlerror();
    using InitializeFunction = int (*)();
    const InitializeFunction initialize = ToFunction<InitializeFunction>(::dlsym(injectionHandle, "acltoolInitialize"));
    const char* symbolError = ::dlerror();
    if (symbolError != nullptr || initialize == nullptr) {
        std::fprintf(
            stderr, "[real_hardware_callback_app] dlsym failed: %s\n",
            symbolError == nullptr ? "acltoolInitialize is null" : symbolError);
        return 7;
    }
    using ConfigureFunction = void (*)(const char*, const char*);
    const auto configure = ToFunction<ConfigureFunction>(::dlsym(injectionHandle, "NpuComputeTestConfigure"));
    if (configure == nullptr) {
        return 7;
    }
    configure(outputDirectory.c_str(), SECTIONS);
    const int initializeResult = initialize();
    if (initializeResult != ACLPTI_SUCCESS) {
        std::fprintf(stderr, "[real_hardware_callback_app] acltoolInitialize failed: %d\n", initializeResult);
        return 8;
    }
    // On later failures, process exit stops the collector before the OS tears
    // down ACL. The success path performs explicit cleanup after publication.
    aclRuntime.DisableAutomaticCleanup();

    result = aclrtSetDevice(DEVICE_ID);
    if (result != ACL_SUCCESS) {
        std::fprintf(stderr, "[real_hardware_callback_app] aclrtSetDevice failed: %d\n", static_cast<int>(result));
        return 9;
    }
    aclRuntime.MarkDeviceSet();

    if (!EmitSuccessfulExit(callbackId)) {
        std::fprintf(stderr, "[real_hardware_callback_app] callback event was not dispatched\n");
        return 10;
    }
    if (!HardwareInfoIsComplete(outputDirectory)) {
        std::fprintf(
            stderr, "[real_hardware_callback_app] HardwareInfo.jsonl is incomplete after callback cbid=%u\n",
            callbackId);
        return 11;
    }
    std::fprintf(
        stderr, "[real_hardware_callback_app] HardwareInfo.jsonl available after callback cbid=%u\n", callbackId);
    if (!aclRuntime.Cleanup()) {
        return 12;
    }
    std::fprintf(stderr, "[real_hardware_callback_app] Device cleanup completed\n");
    return 0;
}
