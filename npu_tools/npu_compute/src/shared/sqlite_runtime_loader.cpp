/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "shared/sqlite_runtime_loader.h"
#include <boost/filesystem.hpp>
#include <functional>
#include <string>
#include <system_error>
#include <limits>

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace npucompute::sqlite {
namespace {
namespace fs = boost::filesystem;
#ifdef _WIN32
constexpr std::size_t kInitialModulePathCapacity = 256;
constexpr std::size_t kInitialErrorMessageCapacity = 256;
constexpr std::size_t kBufferCapacityGrowthFactor = 2;
#endif

struct Candidate {
    fs::path path;
    bool systemAbi = false;
};

#ifdef _WIN32
using Library = HMODULE;
std::string SystemError()
{
    const auto code = GetLastError();
    const auto prefix = "Windows error " + std::to_string(code);
    std::string message(kInitialErrorMessageCapacity, '\0');
    for (;;) {
        const auto size = FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, message.data(),
            static_cast<DWORD>(message.size()), nullptr);
        if (size != 0) {
            message.resize(size);
            return prefix + ": " + message;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
            message.size() > std::numeric_limits<DWORD>::max() / kBufferCapacityGrowthFactor) {
            return prefix;
        }
        message.resize(message.size() * kBufferCapacityGrowthFactor);
    }
}
fs::path ModuleDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module)) {
        return {};
    }
    std::vector<wchar_t> path(kInitialModulePathCapacity);
    for (;;) {
        const auto size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0) {
            return {};
        }
        if (size < path.size()) {
            return fs::path(std::wstring(path.data(), size)).parent_path();
        }
        path.resize(path.size() * kBufferCapacityGrowthFactor);
    }
}
Library OpenLibrary(const Candidate& candidate)
{
    const auto flags = candidate.path.is_absolute() ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 :
                       candidate.systemAbi          ? LOAD_LIBRARY_SEARCH_SYSTEM32 :
                                                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    return LoadLibraryExW(candidate.path.c_str(), nullptr, flags);
}
void CloseLibrary(Library library) { FreeLibrary(library); }
void* FindSymbol(Library library, const char* name) { return reinterpret_cast<void*>(GetProcAddress(library, name)); }
#else
using Library = void*;
std::string SystemError()
{
    const auto* message = dlerror();
    return message ? message : "dynamic loader did not provide an error message";
}
fs::path ModuleDirectory()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&ModuleDirectory), &info) == 0 || !info.dli_fname) {
        return {};
    }
    boost::system::error_code error;
    const auto path = fs::absolute(info.dli_fname, error);
    return error ? fs::path{} : path.parent_path();
}
Library OpenLibrary(const Candidate& candidate) { return dlopen(candidate.path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void CloseLibrary(Library library) { dlclose(library); }
void* FindSymbol(Library library, const char* name)
{
    dlerror();
    return dlsym(library, name);
}
#endif

std::vector<Candidate> Candidates()
{
    std::vector<Candidate> result;
    const auto directory = ModuleDirectory();
#ifdef _WIN32
    result.push_back({L"winsqlite3.dll", true});
    if (!directory.empty()) {
        result.push_back({directory / L"sqlite3.dll", false});
    }
    result.push_back({L"sqlite3.dll", false});
#else
    if (!directory.empty()) {
        result.push_back({directory / "libsqlite3.so.0"});
        result.push_back({directory / "libsqlite3.so"});
    }
    result.push_back({"libsqlite3.so.0"});
    result.push_back({"libsqlite3.so"});
#endif
    return result;
}

void* Symbol(Library library, const char* name)
{
    auto* address = FindSymbol(library, name);
    if (!address) {
        throw std::runtime_error("resolve symbol " + std::string(name) + ": " + SystemError());
    }
    return address;
}

template <class Result, class... Args>
void Resolve(Library library, const char* name, std::function<Result(Args...)>& target, bool systemAbi)
{
    auto* address = Symbol(library, name);
#ifdef _WIN32
    if (systemAbi) {
        target = reinterpret_cast<Result(__stdcall*)(Args...)>(address);
        return;
    }
    target = reinterpret_cast<Result(__cdecl*)(Args...)>(address);
#else
    (void)systemAbi;
    target = reinterpret_cast<Result (*)(Args...)>(address);
#endif
}

void ResolveCallbacks(Library library, Api& api, bool systemAbi)
{
    auto* exec = Symbol(library, "sqlite3_exec");
    auto* bind = Symbol(library, "sqlite3_bind_text");
#ifdef _WIN32
    if (systemAbi) {
        using Callback = int(__stdcall*)(void*, int, char**, char**);
        using Destructor = void(__stdcall*)(void*);
        const auto execFunction =
            reinterpret_cast<int(__stdcall*)(sqlite3*, const char*, Callback, void*, char**)>(exec);
        const auto bindFunction =
            reinterpret_cast<int(__stdcall*)(sqlite3_stmt*, int, const char*, int, Destructor)>(bind);
        api.exec = [execFunction](sqlite3* db, const char* sql) {
            return execFunction(db, sql, nullptr, nullptr, nullptr);
        };
        api.bindText = [bindFunction](sqlite3_stmt* stmt, int column, const char* text, int length) {
            return bindFunction(stmt, column, text, length, reinterpret_cast<Destructor>(kTransientDestructor));
        };
        return;
    }
    using Callback = int(__cdecl*)(void*, int, char**, char**);
    using Destructor = void(__cdecl*)(void*);
    const auto execFunction = reinterpret_cast<int(__cdecl*)(sqlite3*, const char*, Callback, void*, char**)>(exec);
    const auto bindFunction = reinterpret_cast<int(__cdecl*)(sqlite3_stmt*, int, const char*, int, Destructor)>(bind);
#else
    (void)systemAbi;
    using Callback = int (*)(void*, int, char**, char**);
    using Destructor = void (*)(void*);
    const auto execFunction = reinterpret_cast<int (*)(sqlite3*, const char*, Callback, void*, char**)>(exec);
    const auto bindFunction = reinterpret_cast<int (*)(sqlite3_stmt*, int, const char*, int, Destructor)>(bind);
#endif
    api.exec = [execFunction](sqlite3* db, const char* sql) {
        return execFunction(db, sql, nullptr, nullptr, nullptr);
    };
    api.bindText = [bindFunction](sqlite3_stmt* stmt, int column, const char* text, int length) {
        return bindFunction(stmt, column, text, length, reinterpret_cast<Destructor>(kTransientDestructor));
    };
}

struct Runtime {
    Api api;
    std::string path;
    std::string error;
};

Runtime LoadRuntime()
{
    Runtime result;
    result.error = "SQLite runtime loading failed; database output is unavailable. Attempts:";
    for (const auto& candidate : Candidates()) {
        // Failed candidates are closed; successful handles stay loaded so all
        // connections, statements and their cleanup can outlive static objects.
        const auto library = OpenLibrary(candidate);
        if (!library) {
            result.error += " [" + candidate.path.string() + ": open library: " + SystemError() + "]";
            continue;
        }
        try {
            Api api;
            Resolve(library, "sqlite3_open_v2", api.openV2, candidate.systemAbi);
            Resolve(library, "sqlite3_close", api.close, candidate.systemAbi);
            Resolve(library, "sqlite3_close_v2", api.closeV2, candidate.systemAbi);
            Resolve(library, "sqlite3_prepare_v2", api.prepareV2, candidate.systemAbi);
            Resolve(library, "sqlite3_step", api.step, candidate.systemAbi);
            Resolve(library, "sqlite3_finalize", api.finalize, candidate.systemAbi);
            Resolve(library, "sqlite3_reset", api.reset, candidate.systemAbi);
            Resolve(library, "sqlite3_clear_bindings", api.clearBindings, candidate.systemAbi);
            Resolve(library, "sqlite3_bind_int64", api.bindInt64, candidate.systemAbi);
            Resolve(library, "sqlite3_bind_double", api.bindDouble, candidate.systemAbi);
            Resolve(library, "sqlite3_bind_null", api.bindNull, candidate.systemAbi);
            Resolve(library, "sqlite3_errmsg", api.errmsg, candidate.systemAbi);
            Resolve(library, "sqlite3_column_type", api.columnType, candidate.systemAbi);
            Resolve(library, "sqlite3_column_int64", api.columnInt64, candidate.systemAbi);
            Resolve(library, "sqlite3_column_double", api.columnDouble, candidate.systemAbi);
            Resolve(library, "sqlite3_column_text", api.columnText, candidate.systemAbi);
            Resolve(library, "sqlite3_column_bytes", api.columnBytes, candidate.systemAbi);
            Resolve(library, "sqlite3_column_count", api.columnCount, candidate.systemAbi);
            ResolveCallbacks(library, api, candidate.systemAbi);
            result.api = std::move(api);
            result.path = candidate.path.string();
            result.error.clear();
            return result;
        } catch (const std::exception& failure) {
            result.error += " [" + candidate.path.string() + ": " + failure.what() + "]";
            CloseLibrary(library);
        }
    }
    result.error += " CSV, disassembly and symbolizer results can still be published.";
    return result;
}

const Runtime& GetRuntime()
{
    // C++ static initialization is thread safe. Retaining this small object also
    // keeps its callables valid during process shutdown. Failed loads are cached.
    static const auto* runtime = new Runtime(LoadRuntime());
    return *runtime;
}
} // namespace

const Api& GetApi()
{
    const auto& runtime = GetRuntime();
    if (!runtime.error.empty()) {
        throw std::runtime_error(runtime.error);
    }
    return runtime.api;
}

const std::string& LibraryPath()
{
    GetApi();
    return GetRuntime().path;
}
} // namespace npucompute::sqlite
