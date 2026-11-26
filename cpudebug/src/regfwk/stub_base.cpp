/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file stub_base.cpp
 * \brief
 */
#include <sys/stat.h>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include "securec.h"
#include "stub_def.h"

int64_t block_idx = 0;
int64_t block_num = 8;
int64_t g_ubBase = 0;
uint64_t g_tilingKey = 0;
int32_t g_coreType = 0; // mix = 0; cube = 1; vec = 2;
int32_t g_matmulCount = 0;
std::map<std::string, uint64_t>& GetArgVal()
{
    static std::map<std::string, uint64_t> instance;
    return instance;
}

int32_t g_taskRation = 2;
uint32_t g_threadDimX = 1u;
uint32_t g_threadDimY = 1u;
uint32_t g_threadDimZ = 1u;
thread_local uint32_t g_threadIdxX = 0u;
thread_local uint32_t g_threadIdxY = 0u;
thread_local uint32_t g_threadIdxZ = 0u;
int32_t sub_block_idx = 0;
std::string& GetStrCoreType()
{
    static std::string config = "mix";
    return config;
}
uint64_t* g_workspaceSharedPtr = nullptr;
uint64_t g_fullSizeOfWorkspace = 0;
KernelMode g_kernelMode = KernelMode::MIX_MODE;
SocVersion g_socVersion = SocVersion::VER_MAX;
std::vector<ArgInfoT>& GetArgInfoList()
{
    static std::vector<ArgInfoT> instance;
    return instance;
}
std::vector<std::string>& GetValidArgTypeList()
{
    static std::vector<std::string> instance;
    return instance;
}
std::vector<std::string>& GetTmpFileName()
{
    static std::vector<std::string> instance;
    return instance;
}
std::vector<int32_t>& GetProcessId()
{
    static std::vector<int32_t> instance;
    return instance;
}
int32_t g_mainPid = 0;
int32_t g_processNum = 0;
uint64_t g_fixpipeNdNzParam = 0;

#ifdef TASK_RATION
int32_t g_taskRation = TASK_RATION;
#endif

namespace AscendC {
const int MIX_TYPE = 0;
const int AIC_TYPE = 1;
const int AIV_TYPE = 2;
const uint64_t ONE_GIGABYTE = 1024 * 1024 * 1024;
uint8_t g_fftsGlobalLock = 0;
uint8_t (*g_syncCounterEachcore)[FLAG_NUM] = nullptr;
uint8_t (*g_syncCounterFfts)[FLAG_NUM] = nullptr;
const uint64_t DOUBLE = 2;
bool g_isVdeq = false;

void SetKernelMode(KernelMode mode)
{
    g_kernelMode = mode;
    if (g_kernelMode == KernelMode::MIX_AIC_1_1) {
        g_taskRation = 1;
    }
}

void AddNameArg(const char* name, unsigned long val) { GetArgVal().emplace(name, val); }

unsigned long GetNameArg(const char* name)
{
    if (name == nullptr) {
        std::cerr << "GetNameArg Error: name must not be null." << std::endl;
        raise(SIGABRT);
        return 0;
    }
    const auto iter = GetArgVal().find(name);
    if (iter == GetArgVal().end()) {
        std::cerr << "GetNameArg Error: name argument not found: " << name << std::endl;
        raise(SIGABRT);
        return 0;
    }
    return static_cast<unsigned long>(iter->second);
}

std::string BuildExp(uint64_t val)
{
    char buff[256];
    std::string name = "";
    uint64_t offset = ARG_STEP;

    for (auto it : GetArgVal()) {
        if (offset > val - it.second) {
            offset = val - it.second;
            name = it.first;
        }
    }
    if (offset != 0) {
        int ret = snprintf_s(buff, sizeof(buff), sizeof(buff), "(((uint64_t)%s) + 0x%lx)", name.c_str(), offset);
        if (ret <= 0) {
            raise(SIGABRT);
        }
    } else {
        int ret = snprintf_s(buff, sizeof(buff), sizeof(buff), "%s", name.c_str());
        if (ret <= 0) {
            raise(SIGABRT);
        }
    }
    return std::string(buff);
}

bool FileExists(std::string fileName)
{
    struct stat buffer;
    return stat(fileName.c_str(), &buffer) == 0;
}

uint64_t GetTime()
{
    struct timespec ts {};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t milliseconds = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
    return milliseconds;
}

std::string GetFileName()
{
    std::string fileName = "/tmp/tmpfile_" + std::to_string(GetTime()) + "_" + std::to_string(getpid());
    while (FileExists(fileName)) {
        fileName = "/tmp/tmpfile_" + std::to_string(GetTime()) + "_" + std::to_string(getpid());
    }
    return fileName;
}

void HandleError(bool condition, const std::string& message, int fd)
{
    if (condition) {
        std::cerr << "GmAlloc Error: " << message << std::endl;
        close(fd);
        raise(SIGABRT);
    }
}

/**
 * GM memory structure:
 * protect user tail 4k memory
 *
 * 0                      4K  <8K                          -4K                   END
 * +----------------------+----+----------------------------+----------------------+
 * |  4K HEADER READ ONLY |////|<------- USER SIZE -------->| 4K TAIL CANNOT ACCESS|
 * +----------------------+----+----------------------------+----------------------+
 *                             ^                            ^
 *       HEADER                USER START                   USER TAIL 4K ALIGN
 */

void* GmAlloc(size_t size)
{
    size_t pageSize = getpagesize();
    size_t alignedExtraPageSize = pageSize * 2 + pageSize - 1; // 2 pages for header and tail
    if (size > (SIZE_MAX - alignedExtraPageSize)) {
        std::cerr << "GmAlloc Error: input size " << size << " exceeds maximum allowed size "
                  << (SIZE_MAX - alignedExtraPageSize) << " bytes." << std::endl;
        raise(SIGABRT);
    }
    size_t newSize = (size + alignedExtraPageSize) & (~(pageSize - 1)); // align to multiple page size
    std::string fileName = GetFileName();
    int fd = open(fileName.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    HandleError(fd == -1, "Failed to open file: " + std::string(strerror(errno)), fd);
    int res = ftruncate(fd, newSize);
    if (errno == EFBIG) {
        HandleError(res != 0, "The /tmp directory does not have enough space.", fd);
    }
    auto filePtr = mmap(nullptr, newSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    HandleError(filePtr == MAP_FAILED, "Error map file to ptr, error code: " + std::string(strerror(errno)), fd);
    if (static_cast<int>(reinterpret_cast<intptr_t>(filePtr)) == -1) {
        std::cout << "Error map file to ptr, error code: " << errno << std::endl;
        raise(SIGABRT);
    }
    ShmMemT* mem = static_cast<ShmMemT*>(filePtr);
    mem->fd = fd;
    mem->size = newSize;
    mem->magicCode = 0xdeadbeef;
    errno_t ret = strcpy_s(mem->fileName, sizeof(mem->fileName), fileName.c_str());
    HandleError(ret != EOK, "strcpy_s failed, ret = " + std::to_string(ret), fd);
    (void)mprotect(mem, pageSize, PROT_READ);
    (void)mprotect(reinterpret_cast<uint8_t*>(mem) + newSize - pageSize, pageSize, PROT_NONE);
    void* userStart = reinterpret_cast<uint8_t*>(mem) + newSize - pageSize - size;
    void* headerTail = reinterpret_cast<uint8_t*>(mem) + pageSize;
    if (userStart != headerTail) {
        size_t emptySize = (uint8_t*)userStart - (uint8_t*)headerTail;
        if (emptySize > 0) {
            memset_s(headerTail, emptySize, 0xff, emptySize);
        }
    }
    return userStart;
}

void* GmGetHeader(void* ptr)
{
    int pageSize = getpagesize();
    return (void*)(((uint64_t)ptr & (~(pageSize - 1))) - pageSize);
}

uint64_t GmGetUserSize(uint64_t addr)
{
    uint64_t pageSize = getpagesize();
    if (mprotect(reinterpret_cast<void*>(addr & ~(pageSize - 1)), pageSize, PROT_READ | PROT_WRITE) != 0) {
        return 0;
    }
    ShmMemT* mem = static_cast<AscendC::ShmMemT*>(AscendC::GmGetHeader(reinterpret_cast<void*>(addr)));
    size_t size = mem->size;
    uint64_t offset = addr - (addr & ~(pageSize - 1));
    return size - DOUBLE * pageSize - offset;
}

void CheckEmptyGmValied(void* ptr)
{
    int pageSize = getpagesize();
    void* headerTail = (void*)((uint64_t)ptr & (~(pageSize - 1)));
    if (headerTail != ptr) {
        for (uint8_t* tmpPtr = (uint8_t*)headerTail; tmpPtr < (uint8_t*)ptr; tmpPtr++) {
            if (*(uint8_t*)tmpPtr != 0xff) {
                std::cout << "Empty memory is accessed ! or \
                              this memory has been released more than one time."
                          << std::endl;
                raise(SIGABRT);
            }
        }
    }
}

void GmFree(void* ptr)
{
    int pageSize = getpagesize();
    CheckEmptyGmValied(ptr);
    int fd;
    size_t size;
    char file[256];
    ShmMemT* mem = static_cast<ShmMemT*>(GmGetHeader(ptr));
    fd = mem->fd;
    size = mem->size;
    errno_t ret = strcpy_s(file, sizeof(file), mem->fileName);
    if (ret != EOK) {
        std::cout << "strcpy_s failed, ret = " << ret << std::endl;
        raise(SIGABRT);
    }
    munmap(mem, size);
    close(fd);
    remove(file);
    (void)mprotect(mem, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC);
    (void)mprotect(reinterpret_cast<uint8_t*>(mem) + size - pageSize, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC);
}

void CheckGmValied(int argn, uint64_t* argv)
{
    for (int i = 0; i < argn; i++) {
        int ret = mprotect(reinterpret_cast<void*>(argv[i] & 0xfff), 0x1000, PROT_READ | PROT_WRITE);
        if (ret != 0) {
            continue;
        }
        ShmMemT* mem = reinterpret_cast<ShmMemT*>(GmGetHeader((void*)argv[i]));
        if (mem->magicCode != 0xdeadbeef) {
            std::cout << "The address of args was not allocated by AscendC::GmAlloc!" << std::endl;
            raise(SIGABRT);
        }
    }
}

void CheckBlockdimForFfts(uint64_t numBlocks)
{
    (void)(numBlocks);
#if defined(__NPU_ARCH__) && __NPU_ARCH__ == 2201
    if ((g_kernelMode == KernelMode::MIX_MODE && numBlocks > MAX_CORE_NUM_V220) ||
        (g_kernelMode == KernelMode::AIC_MODE && numBlocks > MAX_CORE_NUM_V220) ||
        (g_kernelMode == KernelMode::AIV_MODE && numBlocks > MAX_CORE_NUM_V220 * AIV_IN_GROUP_CORE_NUM)) {
        const int32_t maxCoreNum =
            g_kernelMode == KernelMode::AIV_MODE ? MAX_CORE_NUM_V220 * AIV_IN_GROUP_CORE_NUM : MAX_CORE_NUM_V220;
        std::cout << "The input numBlocks " << numBlocks << " exceeds max core num " << maxCoreNum << " of ascend910B1!"
                  << std::endl;
        raise(SIGABRT);
    }
#endif
}

void CheckNumBlocksForFfts(uint64_t numBlocks) { CheckBlockdimForFfts(numBlocks); }

void SetGCoreType(int type)
{
    if (type < MIX_TYPE || type > AIV_TYPE) {
        std::cout << "Error g_coreType!" << std::endl;
        raise(SIGABRT);
    }
    g_coreType = type;
}

void SetArgInfoList(const std::vector<ArgInfoT>& argInfoList) { GetArgInfoList() = argInfoList; }
} // namespace AscendC
