/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_api.h"
#include "kernel_argument_elf_fixture.h"
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"

#include <cassert>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace fs = boost::filesystem;

namespace {

aclError OriginalBinaryLoad(const void*, size_t, const aclrtBinaryLoadOptions*, aclrtBinHandle* binary)
{
    if (binary == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *binary = reinterpret_cast<aclrtBinHandle>(0x51);
    return ACL_SUCCESS;
}

aclError OriginalResetDevice(int32_t) { return ACL_SUCCESS; }

void Callback(void*, AclsanCallbackDomain, AclsanCallbackId, const void*) {}

} // namespace

static_assert(std::is_same_v<decltype(&aclsanGetDeviceCallStack), AclsanStatus (*)(uint64_t, AclsanDeviceCallStack*)>);
static_assert(ACLSAN_CALL_STACK_MAX_DEPTH == 16U);
static_assert(ACLSAN_FUNCTION_NAME_MAX_BYTES == 4096U);
static_assert(ACLSAN_FILE_NAME_MAX_BYTES == 4096U);

int main()
{
    assert(aclsanGetDeviceCallStack(0x170, nullptr) == ACLSAN_STATUS_ERROR_INVALID_PARAMETER);

    auto result = std::make_unique<AclsanDeviceCallStack>();
    result->binaryId = 99;
    result->pc = 0;
    result->depth = 7;
    result->flags = ACLSAN_CALL_STACK_FLAG_TRUNCATED;
    result->frames[0].line = 42;

    assert(aclsanGetDeviceCallStack(0x170, result.get()) == ACLSAN_STATUS_ERROR_INVALID_STATE);
    assert(result->binaryId == 0);
    assert(result->pc == 0x170);
    assert(result->depth == 0);
    assert(result->flags == ACLSAN_CALL_STACK_FLAG_NONE);
    assert(result->frames[0].line == 0);

    const fs::path work = fs::temp_directory_path() / "aclsan-symbolizer-api-test";
    const fs::path symbolizer = work / "tools/mssanitizer/bin/llvm-symbolizer";
    fs::remove_all(work);
    fs::create_directories(symbolizer.parent_path());
    {
        std::ofstream script(symbolizer.string());
        script << "#!/bin/sh\n"
               << "printf 'CopyIn\\n/src/kernel.asc:46:5\\nAddKernel\\n/src/kernel.asc:58:1\\n'\n";
    }
    fs::permissions(symbolizer, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exe);
    const char* ascendHome = std::getenv("ASCEND_HOME_PATH");
    const bool hadAscendHome = ascendHome != nullptr;
    const std::string savedAscendHome = hadAscendHome ? ascendHome : "";
    assert(setenv("ASCEND_HOME_PATH", work.c_str(), 1) == 0);

    assert(RuntimeStubSetOriginFunction("aclrtBinaryLoadFromData", &OriginalBinaryLoad) == ACL_SUCCESS);
    assert(RuntimeStubSetOriginFunction("aclrtResetDevice", &OriginalResetDevice) == ACL_SUCCESS);
    AclsanSubscriberHandle subscriber = nullptr;
    assert(aclsanSubscribe(&subscriber, &Callback, nullptr) == ACLSAN_STATUS_SUCCESS);
    assert(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC) ==
        ACLSAN_STATUS_SUCCESS);

    const std::vector<uint8_t> image = aclsan::test::MakeKernelArgumentSizeElf(8);
    aclrtBinaryLoadOptions options{};
    aclrtBinHandle binary = nullptr;
    assert(aclrtBinaryLoadFromData(image.data(), image.size(), &options, &binary) == ACL_SUCCESS);
    assert(binary == reinterpret_cast<aclrtBinHandle>(0x51));
    assert(aclsanGetDeviceCallStack(0x170, result.get()) == ACLSAN_STATUS_SUCCESS);
    assert(result->binaryId != 0);
    assert(result->pc == 0x170);
    assert(result->depth == 2);
    assert(result->frames[0].line == 46);
    assert(result->frames[0].column == 5);
    assert(std::string(result->frames[0].functionName) == "CopyIn");
    assert(std::string(result->frames[0].fileName) == "/src/kernel.asc");

    assert(aclrtResetDevice(0) == ACL_SUCCESS);
    assert(aclsanGetDeviceCallStack(0x170, result.get()) == ACLSAN_STATUS_ERROR_INVALID_STATE);
    assert(aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS);
    fs::remove_all(work);
    if (hadAscendHome) {
        assert(setenv("ASCEND_HOME_PATH", savedAscendHome.c_str(), 1) == 0);
    } else {
        assert(unsetenv("ASCEND_HOME_PATH") == 0);
    }
    return 0;
}
