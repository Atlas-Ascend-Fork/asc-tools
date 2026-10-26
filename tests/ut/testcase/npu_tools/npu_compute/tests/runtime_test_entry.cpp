/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute/npu_compute.h"

#include "runtime/npu_compute_runtime.h"
#include "artifact_sink.h"

#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>

namespace {

class FileArtifactSink final : public npucompute::ArtifactSink {
public:
    void Begin(const std::string& name) override
    {
        path_ = root / name;
        boost::filesystem::create_directories(path_.parent_path());
        output_.open(path_.string(), std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("open test artifact failed");
        }
    }
    void Write(std::string_view bytes) override
    {
        output_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output_) {
            throw std::runtime_error("write test artifact failed");
        }
    }
    void Commit(std::uint64_t) override
    {
        output_.close();
        if (output_.fail()) {
            throw std::runtime_error("close test artifact failed");
        }
    }
    void Abort() noexcept override
    {
        output_.close();
        boost::system::error_code ignored;
        boost::filesystem::remove(path_, ignored);
    }
    boost::filesystem::path root;

private:
    boost::filesystem::path path_;
    std::ofstream output_;
};

FileArtifactSink g_sink;
npucompute::RuntimeConfig g_config;

std::once_flag g_initialize_once;
std::mutex g_lifecycle_mutex;
int g_initialize_result = npucompute::kInitializeFailed;
int g_shutdown_result = 0;
bool g_shutdown = false;

void StopRuntimeAtExit() noexcept { npucompute::NpuComputeRuntime::Instance().Stop(); }

void InitializeOnce() noexcept
{
    try {
        npucompute::NpuComputeRuntime& runtime = npucompute::NpuComputeRuntime::Instance();
        g_initialize_result = runtime.Initialize(g_config, g_sink, {});
        if (g_initialize_result == 0 && std::atexit(StopRuntimeAtExit) != 0) {
            std::fprintf(stderr, "[libnpu-compute] register process exit handler failed\n");
            runtime.Stop();
            g_initialize_result = npucompute::kInitializeFailed;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[libnpu-compute] initialization failed: %s\n", error.what());
        g_initialize_result = npucompute::kInitializeFailed;
    } catch (...) {
        std::fprintf(stderr, "[libnpu-compute] initialization failed: unknown exception\n");
        g_initialize_result = npucompute::kInitializeFailed;
    }
}

} // namespace

extern "C" NPU_COMPUTE_EXPORT void NpuComputeTestConfigure(const char* output, const char* sections)
{
    g_sink.root = output;
    g_config.sections.clear();
    std::istringstream input(sections);
    for (std::string section; std::getline(input, section, ',');) {
        g_config.sections.push_back(section);
    }
}

extern "C" NPU_COMPUTE_EXPORT int acltoolInitialize()
{
    try {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        if (g_shutdown) {
            return npucompute::kInitializeFailed;
        }
        std::call_once(g_initialize_once, InitializeOnce);
    } catch (...) {
        return npucompute::kInitializeFailed;
    }
    return g_initialize_result;
}

extern "C" NPU_COMPUTE_EXPORT int acltoolShutdown()
{
    try {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        if (!g_shutdown) {
            g_shutdown_result = npucompute::NpuComputeRuntime::Instance().ShutdownAfterPtiDrain();
            g_shutdown = true;
        }
        return g_shutdown_result;
    } catch (...) {
        return npucompute::kInitializeFailed;
    }
}
