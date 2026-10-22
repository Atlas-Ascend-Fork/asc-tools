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
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
std::mutex mutex;
uintptr_t nextHandle = 0x1000;
std::map<aclrtFuncHandle, std::pair<aclrtBinHandle, uint64_t>> functions;
std::map<aclrtBinHandle, bool> alive;
std::map<aclrtBinHandle, size_t> references;
std::atomic<size_t> loads{0}, launches{0}, unloads{0};
std::atomic<int> syncResult{0};
std::atomic<int> unloadResult{0};
std::atomic<int> lookupResult{0};
thread_local uintptr_t contextValue = 0xabc;
aclrtBinHandle parent = nullptr;
std::string filePath;
bool fileMode = false;
bool returnNullBinary = false;
bool returnDuplicateBinary = false;

std::vector<uint8_t> MultiElf()
{
    auto image = aclsan::test::MakeKernelArgumentSizeElf(16);
    Elf64_Ehdr header;
    std::memcpy(&header, image.data(), sizeof(header));
    Elf64_Shdr original[4];
    std::memcpy(original, image.data() + header.e_shoff, sizeof(original));
    std::vector<Elf64_Shdr> sections{original[0], original[1], original[2]};
    std::string names("\0.shstrtab\0__CCE_KernelArgSize\0", 31);
    for (uint64_t key : {uint64_t(0), uint64_t(1), std::numeric_limits<uint64_t>::max()}) {
        auto section = original[3];
        section.sh_name = names.size();
        names += ".ascend.meta.K_" + std::to_string(key);
        names.push_back('\0');
        sections.push_back(section);
    }
    sections[1].sh_offset = image.size();
    sections[1].sh_size = names.size();
    image.insert(image.end(), names.begin(), names.end());
    header.e_shoff = image.size();
    header.e_shnum = sections.size();
    const auto* begin = reinterpret_cast<const uint8_t*>(sections.data());
    image.insert(image.end(), begin, begin + sections.size() * sizeof(Elf64_Shdr));
    std::memcpy(image.data(), &header, sizeof(header));
    return image;
}

aclError Load(const void*, size_t, const aclrtBinaryLoadOptions* options, aclrtBinHandle* binary)
{
    assert(options && options->numOpt == 1);
    assert(
        (options->options[0].type == ACL_RT_BINARY_LOAD_OPT_LAZY_LOAD && options->options[0].value.isLazyLoad == 1) ||
        (options->options[0].type == ACL_RT_BINARY_LOAD_OPT_MAGIC &&
         options->options[0].value.magic == ACL_RT_BINARY_MAGIC_ELF_AICORE));
    if (returnNullBinary) {
        *binary = nullptr;
        return ACL_SUCCESS;
    }
    std::lock_guard<std::mutex> lock(mutex);
    *binary = returnDuplicateBinary ? parent : reinterpret_cast<void*>(nextHandle++);
    ++references[*binary];
    alive[*binary] = true;
    ++loads;
    return ACL_SUCCESS;
}
aclError FileLoad(const char* path, aclrtBinaryLoadOptions* options, aclrtBinHandle* binary)
{
    assert(access(path, F_OK) == 0);
    filePath = path;
    return Load(nullptr, 0, options, binary);
}
aclError Lookup(aclrtBinHandle binary, uint64_t entry, aclrtFuncHandle* function)
{
    if (binary != parent && lookupResult != 0) {
        return lookupResult;
    }
    if (binary != parent && entry == 2) {
        return ACL_ERROR_INVALID_PARAM;
    }
    std::lock_guard<std::mutex> lock(mutex);
    assert(alive.at(binary));
    *function = reinterpret_cast<void*>(nextHandle++);
    functions[*function] = {binary, entry};
    return ACL_SUCCESS;
}
aclError Named(aclrtBinHandle binary, const char*, aclrtFuncHandle* function) { return Lookup(binary, 0, function); }
aclError Unload(aclrtBinHandle binary)
{
    if (unloadResult != 0)
        return unloadResult;
    std::lock_guard<std::mutex> lock(mutex);
    assert(alive.at(binary));
    alive[binary] = --references.at(binary) != 0;
    ++unloads;
    return ACL_SUCCESS;
}
aclError Launch(
    aclrtFuncHandle function, uint32_t, aclrtStream, aclrtLaunchKernelCfg*, void* args, size_t bytes,
    aclrtPlaceHolderInfo*, size_t)
{
    std::lock_guard<std::mutex> lock(mutex);
    assert(functions.at(function).first != parent && alive.at(functions.at(function).first));
    assert(bytes == 24 && args != nullptr);
    void* trace = nullptr;
    std::memcpy(&trace, static_cast<char*>(args) + 16, sizeof(trace));
    assert(trace != nullptr);
    ++launches;
    return ACL_SUCCESS;
}
aclError ArrayLaunch(void* function, uint32_t blocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void** args)
{
    char packed[24]{};
    std::memcpy(packed + 16, args[0], sizeof(void*));
    return Launch(function, blocks, stream, cfg, packed, sizeof(packed), nullptr, 0);
}
aclError ParamCount(const void*, size_t* count)
{
    *count = 1;
    return ACL_SUCCESS;
}
aclError ParamInfo(const void*, size_t, size_t* offset, size_t* bytes)
{
    *offset = 16;
    *bytes = 8;
    return ACL_SUCCESS;
}
aclError Sync(aclrtStream) { return syncResult; }
void Callback(void*, AclsanCallbackDomain, AclsanCallbackId, const void*) {}
size_t TuneCount(const std::string& key)
{
    std::ifstream input(std::getenv("DBI_FAKE_LOG"));
    size_t count = 0;
    for (std::string line; std::getline(input, line);) {
        if (line.find("bisheng-tune <") == 0 && line.find(key) != std::string::npos)
            ++count;
    }
    return count;
}
} // namespace

extern "C" aclError aclrtGetCurrentContext(aclrtContext* context)
{
    *context = reinterpret_cast<void*>(contextValue);
    return ACL_SUCCESS;
}

extern "C" aclError AclsanMultiEntryFunctionGetBinary(aclrtFuncHandle function, aclrtBinHandle* binary) __asm__(
    "aclrtFunctionGetBinary");

extern "C" aclError AclsanMultiEntryFunctionGetBinary(aclrtFuncHandle function, aclrtBinHandle* binary)
{
    std::lock_guard<std::mutex> lock(mutex);
    *binary = functions.at(function).first;
    return ACL_SUCCESS;
}

extern "C" aclError AclsanMultiEntryGetFunctionName(aclrtFuncHandle, uint32_t, char*) __asm__("aclrtGetFunctionName");

extern "C" aclError AclsanMultiEntryGetFunctionName(aclrtFuncHandle, uint32_t bytes, char* name)
{
    assert(bytes >= 4);
    std::memcpy(name, "K_0", 4);
    return ACL_SUCCESS;
}

aclError Symbol(const void* symbol, aclrtFuncHandle* function)
{
    *function = const_cast<void*>(symbol);
    return ACL_SUCCESS;
}

int main(int argc, char**)
{
    fileMode = argc > 1;
#define SET(name, fn) assert(RuntimeStubSetOriginFunction(name, &fn) == ACL_SUCCESS)
    SET("aclrtBinaryLoadFromData", Load);
    SET("aclrtBinaryLoadFromFile", FileLoad);
    SET("aclrtBinaryGetFunctionByEntry", Lookup);
    SET("aclrtBinaryGetFunction", Named);
    SET("aclrtGetFuncBySymbol", Symbol);
    SET("aclrtBinaryUnLoad", Unload);
    SET("aclrtLaunchKernelWithHostArgs", Launch);
    SET("aclrtLaunchKernelWithArgsArray", ArrayLaunch);
    SET("aclrtFunctionGetParamCount", ParamCount);
    SET("aclrtFunctionGetParamInfo", ParamInfo);
    SET("aclrtSynchronizeStream", Sync);
    assert(RuntimeStubSetSocName("Ascend950PR_9589") == ACL_SUCCESS);
    assert(acltoolHookInit() == ACL_SUCCESS);
    assert(aclrtSetDevice(0) == ACL_SUCCESS);
    AclsanSubscriberHandle subscriber{};
    assert(aclsanSubscribe(&subscriber, Callback, nullptr) == ACLSAN_STATUS_SUCCESS);
    assert(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC) ==
        ACLSAN_STATUS_SUCCESS);
    auto image = MultiElf();
    aclrtBinaryLoadOption option{ACL_RT_BINARY_LOAD_OPT_LAZY_LOAD, {1}};
    aclrtBinaryLoadOptions options{&option, 1};
    char path[] = "/tmp/aclsan-multi-XXXXXX";
    if (fileMode) {
        int fd = mkstemp(path);
        assert(fd >= 0 && write(fd, image.data(), image.size()) == static_cast<ssize_t>(image.size()));
        assert(close(fd) == 0);
        assert(aclrtBinaryLoadFromFile(path, &options, &parent) == ACL_SUCCESS);
        assert(unlink(path) == 0);
    } else {
        assert(aclrtBinaryLoadFromData(image.data(), image.size(), &options, &parent) == ACL_SUCCESS);
    }
    assert(loads == 1 && TuneCount("") == 0);
    image.clear();
    option.value.isLazyLoad = 0; // Deferred loads must not borrow caller storage.
    auto stream = reinterpret_cast<aclrtStream>(0x77);
    aclrtFuncHandle zero{}, one{}, large{}, missing{}, named{};
    assert(aclrtBinaryGetFunctionByEntry(parent, 0, &zero) == 0);
    assert(aclrtBinaryGetFunctionByEntry(parent, 1, &one) == 0);
    assert(aclrtBinaryGetFunctionByEntry(parent, UINT64_MAX, &large) == 0);
    assert(aclrtBinaryGetFunctionByEntry(parent, 2, &missing) == 0);
    assert(aclrtBinaryGetFunction(parent, "K_0", &named) == 0);
    auto run = [&](aclrtFuncHandle f) {
        assert(aclrtSetDevice(0) == ACL_SUCCESS);
        assert(aclrtLaunchKernelWithHostArgs(f, 1, stream, nullptr, nullptr, 0, nullptr, 0) == 0);
    };
    std::thread first([&] { run(zero); });
    std::thread second([&] { run(zero); });
    first.join();
    second.join();
    assert(TuneCount("<--tiling-key=0>") == 1);
    run(one);
    run(large);
    run(zero);
    assert(TuneCount("<--tiling-key=1>") == 1);
    assert(TuneCount("<--tiling-key=18446744073709551615>") == 1);
    assert(aclrtLaunchKernelWithArgsArray(zero, 1, stream, nullptr, nullptr) == 0);
    assert(TuneCount("<--tiling-key=0>") == 2); // ABI-specific loaded variants.
    run(named);                                 // Lazy compiler name lookup remains supported.
    const auto launchesBeforeTuneFailure = launches.load();
    const auto tunesBeforeTuneFailure = TuneCount("<--tiling-key=18446744073709551615>");
    contextValue = 0xbad;
    assert(setenv("DBI_FAKE_FAIL", "bisheng-tune", 1) == 0);
    assert(aclrtLaunchKernelWithHostArgs(large, 1, stream, nullptr, nullptr, 0, nullptr, 0) != ACL_SUCCESS);
    assert(unsetenv("DBI_FAKE_FAIL") == 0);
    assert(launches == launchesBeforeTuneFailure);
    assert(TuneCount("<--tiling-key=18446744073709551615>") == tunesBeforeTuneFailure + 1);
    contextValue = 0xabc;
    const auto before = launches.load();
    assert(aclrtLaunchKernelWithHostArgs(missing, 1, stream, nullptr, nullptr, 0, nullptr, 0) != 0);
    assert(launches == before);
    assert(aclrtLaunchKernelWithHostArgs(missing, 1, stream, nullptr, nullptr, 0, nullptr, 0) != 0);
    // Missing entry metadata is rejected during symbol selection, before Tune.
    assert(TuneCount("<--tiling-key=2>") == 0);
    unloadResult = 82;
    const auto unloadedFailures = unloads.load();
    assert(aclrtBinaryUnLoad(parent) == 82 && unloads == unloadedFailures);
    run(zero);
    assert(TuneCount("<--tiling-key=0>") == 2);
    unloadResult = 0;
    assert(aclrtBinaryUnLoad(parent) == 0 && unloads == unloadedFailures + 1);
    syncResult = 81;
    assert(aclrtSynchronizeStream(stream) == 81 && unloads == loads);
    syncResult = 0;
    assert(aclrtSynchronizeStream(stream) == 0);
    assert(unloads == loads);
    if (fileMode)
        assert(access(filePath.c_str(), F_OK) != 0);
    // Reusing the Runtime handle must produce a fresh binary generation.
    const auto tunes = TuneCount("<--tiling-key=0>");
    nextHandle = reinterpret_cast<uintptr_t>(parent);
    image = MultiElf();
    option.value.isLazyLoad = 1;
    assert(aclrtBinaryLoadFromData(image.data(), image.size(), &options, &parent) == 0);
    assert(aclrtBinaryGetFunctionByEntry(parent, 0, &zero) == 0);
    run(zero);
    assert(TuneCount("<--tiling-key=0>") == tunes + 1);
    assert(aclrtSynchronizeStream(stream) == 0);
    assert(aclrtResetDevice(0) == 0);
    run(zero);
    assert(TuneCount("<--tiling-key=0>") == tunes + 2);
    assert(aclrtSynchronizeStream(stream) == 0);
    contextValue = 0xdef;
    stream = reinterpret_cast<aclrtStream>(0x88);
    run(zero);
    assert(TuneCount("<--tiling-key=0>") == tunes + 3);
    assert(aclrtSynchronizeStream(stream) == 0);
    contextValue = 0xabc;
    stream = reinterpret_cast<aclrtStream>(0x77);
    run(zero);
    assert(TuneCount("<--tiling-key=0>") == tunes + 3);
    assert(aclrtSynchronizeStream(stream) == 0);
    assert(aclrtBinaryUnLoad(parent) == 0);
    contextValue = 0xdef;
    assert(aclrtSynchronizeStream(reinterpret_cast<aclrtStream>(0x88)) == 0);
    assert(unloads == loads);
    image = MultiElf();
    assert(aclrtBinaryLoadFromData(image.data(), image.size(), &options, &parent) == ACL_SUCCESS);
    returnDuplicateBinary = true;
    aclrtBinHandle duplicate = nullptr;
    const auto beforeDuplicateRollback = unloads.load();
    assert(aclrtBinaryLoadFromData(image.data(), image.size(), &options, &duplicate) != ACL_SUCCESS);
    assert(duplicate == nullptr && alive.at(parent) && references.at(parent) == 1);
    assert(unloads == beforeDuplicateRollback + 1);
    returnDuplicateBinary = false;
    assert(aclrtBinaryGetFunctionByEntry(parent, 0, &zero) == ACL_SUCCESS);
    const auto beforeFailedLookup = unloads.load();
    lookupResult = 83;
    assert(aclrtLaunchKernelWithHostArgs(zero, 1, stream, nullptr, nullptr, 0, nullptr, 0) == 83);
    assert(unloads == beforeFailedLookup + 1);
    lookupResult = 0;
    aclrtBinHandle unrelated = nullptr;
    assert(aclrtBinaryLoadFromData(image.data(), image.size(), &options, &unrelated) == ACL_SUCCESS);
    aclrtFuncHandle symbolFunction = nullptr;
    assert(aclrtGetFuncBySymbol(zero, &symbolFunction) == ACL_SUCCESS);
    run(symbolFunction);
    assert(aclrtSynchronizeStream(stream) == ACL_SUCCESS);
    assert(aclrtBinaryGetFunctionByEntry(parent, 1, &one) == ACL_SUCCESS);
    const auto beforeFailedUnload = unloads.load();
    lookupResult = 83;
    unloadResult = 82;
    assert(aclrtLaunchKernelWithHostArgs(one, 1, stream, nullptr, nullptr, 0, nullptr, 0) == 83);
    assert(unloads == beforeFailedUnload);
    lookupResult = 0;
    unloadResult = 0;
    assert(aclrtBinaryUnLoad(parent) == ACL_SUCCESS);
    assert(aclrtBinaryUnLoad(unrelated) == ACL_SUCCESS);
    assert(unloads == loads);
    returnNullBinary = true;
    aclrtBinHandle invalidBinary = nullptr;
    assert(aclrtBinaryLoadFromData(image.data(), image.size(), &options, &invalidBinary) != ACL_SUCCESS);
    assert(invalidBinary == nullptr);
    assert(aclrtGetFuncBySymbol(nullptr, &symbolFunction) != ACL_SUCCESS);
    returnNullBinary = false;
    option.type = ACL_RT_BINARY_LOAD_OPT_MAGIC;
    option.value.magic = ACL_RT_BINARY_MAGIC_ELF_AICORE;
    const auto tunesBeforeEager = TuneCount("");
    assert(aclrtBinaryLoadFromData(image.data(), image.size(), &options, &parent) == ACL_SUCCESS);
    assert(TuneCount("") == tunesBeforeEager);
    assert(aclrtBinaryGetFunctionByEntry(parent, 0, &zero) == ACL_SUCCESS);
    run(zero);
    assert(TuneCount("") == tunesBeforeEager + 1);
    assert(aclrtSynchronizeStream(stream) == ACL_SUCCESS);
    assert(aclrtBinaryUnLoad(parent) == ACL_SUCCESS);
    assert(unloads == loads);
    assert(aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS);
    return 0;
}
