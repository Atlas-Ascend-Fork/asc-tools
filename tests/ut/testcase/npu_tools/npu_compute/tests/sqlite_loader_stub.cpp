/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

struct sqlite3;
struct sqlite3_api_routines;

namespace {
using Authorizer = int (*)(void*, int, const char*, const char*, const char*, const char*);
using SetAuthorizer = int (*)(sqlite3*, Authorizer, void*);
SetAuthorizer setAuthorizer = nullptr;

int DenyCommit(void*, int action, const char* first, const char*, const char*, const char*)
{
    constexpr int kTransactionAction = 22;
    constexpr int kDeny = 1;
    return action == kTransactionAction && first && std::strcmp(first, "COMMIT") == 0 ? kDeny : 0;
}

int ConfigureCommitFailure(sqlite3* database, char**, const sqlite3_api_routines*)
{
    return setAuthorizer(database, DenyCommit, nullptr);
}

void EnableCommitFailure(void* library)
{
    using AutoExtension = int (*)(void (*)(void));
    const auto add = reinterpret_cast<AutoExtension>(dlsym(library, "sqlite3_auto_extension"));
    setAuthorizer = reinterpret_cast<SetAuthorizer>(dlsym(library, "sqlite3_set_authorizer"));
    if (!add || !setAuthorizer || add(reinterpret_cast<void (*)(void)>(&ConfigureCommitFailure)) != 0) {
        std::fprintf(stderr, "[sqlite_loader_stub] cannot install COMMIT fault injection\n");
        std::abort();
    }
}
} // namespace

// Test-only fault injection into dlopen. The product has no SQLite override flag.
extern "C" void* dlopen(const char* path, int flags)
{
    using Open = void* (*)(const char*, int);
    static const auto realOpen = reinterpret_cast<Open>(dlsym(RTLD_NEXT, "dlopen"));
    static std::atomic<unsigned> attempts{0};
    const auto* mode = std::getenv("NPU_COMPUTE_SQLITE_STUB_MODE");
    if (!mode || !path || !std::strstr(path, "libsqlite3.so")) {
        return realOpen(path, flags);
    }
    if (std::getenv("NPU_COMPUTE_SQLITE_STUB_EXCLUDE_FIXTURE")) {
        char executable[4096]{};
        const auto length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (length > 0 && std::strstr(executable, "rep_fixture_app")) {
            return realOpen(path, flags);
        }
    }
    const auto attempt = ++attempts;
    std::fprintf(stderr, "[sqlite_loader_stub] attempt=%u library=%s\n", attempt, path);
    if (std::strcmp(mode, "absent") == 0) {
        return realOpen("/npu-compute-test/sqlite-not-installed.so", flags);
    }
    if (std::strcmp(mode, "commit-denied") == 0) {
        auto* library = realOpen(path, flags);
        if (library) {
            EnableCommitFailure(library);
        }
        return library;
    }
    if (std::strcmp(mode, "incomplete") == 0 || std::strcmp(mode, "wrong-architecture") == 0 ||
        (std::strcmp(mode, "first-incomplete") == 0 && attempt == 1)) {
        return realOpen(std::getenv("NPU_COMPUTE_SQLITE_STUB_LIBRARY"), flags);
    }
    return realOpen(path, flags);
}
