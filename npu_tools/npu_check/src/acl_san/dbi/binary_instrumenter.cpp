/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "dbi/binary_instrumenter.h"
#include "securec.h"
#include "dbi/kernel_param_metadata.h"
#include "device_instr/soc_version.h"

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <elf.h>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include "npu_tool_log.h"
#include <iterator>
#include <limits>
#include <sstream>
#include <sys/stat.h>

namespace aclsan {
namespace {

std::atomic<uint64_t> g_requestId{0};

bool EnsurePrivateRuntimeDirectory(const boost::filesystem::path& path, std::string& diagnostic)
{
    boost::system::error_code error;
    boost::filesystem::create_directories(path, error);
    const auto status = boost::filesystem::symlink_status(path, error);
    struct stat metadata {};
    if (error || boost::filesystem::is_symlink(status) || !boost::filesystem::is_directory(status) ||
        lstat(path.c_str(), &metadata) != 0 || metadata.st_uid != geteuid()) {
        diagnostic = "DBI runtime directory is not a private owned directory: " + path.string();
        return false;
    }
    if (chmod(path.c_str(), S_IRWXU) != 0) {
        diagnostic = "cannot restrict DBI runtime directory: " + path.string();
        return false;
    }
    return true;
}

std::string RequestDirectory(const BinaryInstrumentationConfig& config)
{
    const std::string root = config.workDirectory.empty() ? "/tmp" : config.workDirectory;
    std::ostringstream name;
    name << root << "/dbi-instrument-" << static_cast<unsigned long long>(getpid()) << "-" << g_requestId.fetch_add(1);
    return name.str();
}

DbiRequest MakeRequest(
    const BinaryInstrumentationConfig& config, const std::string& input, const std::string& output,
    const std::string& work, uint32_t traceArgumentOffset)
{
    DbiRequest request{};
    request.inputKernel = input;
    request.outputKernel = output;
    request.arch = config.arch;
    request.traceArgumentOffset = traceArgumentOffset;
    request.probeGroups = config.probeGroups;
    request.toolchainRoot = config.toolchainRoot;
    request.workDirectory = work;
    request.cacheDirectory = config.cacheDirectory.empty() ? work + "/probe-cache" : config.cacheDirectory;
    request.strict = config.strict;
    request.keepTemp = config.keepTemp;
    request.useCompleteProbeSet = config.useCompleteProbeSet;
    request.extraTuneArgs = config.tuneArgs;
    return request;
}

bool IsFileRangeValid(size_t offset, size_t bytes, size_t fileBytes)
{
    return offset <= fileBytes && bytes <= fileBytes - offset;
}

bool ResolveTraceArgumentOffset(const void* data, size_t length, uint32_t& traceArgumentOffset, std::string& diagnostic)
{
    if (data == nullptr || length < sizeof(Elf64_Ehdr)) {
        diagnostic = "Device ELF header is missing";
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    Elf64_Ehdr header{};
    if (memcpy_s(&header, sizeof(header), bytes, sizeof(header)) != EOK) {
        diagnostic = "cannot read device ELF header";
        return false;
    }
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shnum == 0 ||
        header.e_shstrndx >= header.e_shnum) {
        diagnostic = "Device ELF section table is unsupported";
        return false;
    }
    const size_t sectionTableBytes = static_cast<size_t>(header.e_shnum) * sizeof(Elf64_Shdr);
    if (!IsFileRangeValid(header.e_shoff, sectionTableBytes, length)) {
        diagnostic = "Device ELF section table is out of bounds";
        return false;
    }
    const auto ReadSection = [&](size_t index, Elf64_Shdr& section) {
        const size_t offset = static_cast<size_t>(header.e_shoff) + index * sizeof(Elf64_Shdr);
        return memcpy_s(&section, sizeof(section), bytes + offset, sizeof(section)) == EOK;
    };
    Elf64_Shdr sectionNames{};
    if (!ReadSection(header.e_shstrndx, sectionNames)) {
        diagnostic = "cannot read device ELF section-name header";
        return false;
    }
    if (!IsFileRangeValid(sectionNames.sh_offset, sectionNames.sh_size, length)) {
        diagnostic = "Device ELF section-name table is out of bounds";
        return false;
    }
    const char* names = reinterpret_cast<const char*>(bytes + sectionNames.sh_offset);
    uint32_t maximumArgumentSize = 0;
    bool hasKernelArgumentSize = false;
    for (size_t index = 0; index < header.e_shnum; ++index) {
        Elf64_Shdr section{};
        if (!ReadSection(index, section)) {
            diagnostic = "cannot read device ELF section header";
            return false;
        }
        if (section.sh_name >= sectionNames.sh_size) {
            continue;
        }
        const size_t nameBytes = sectionNames.sh_size - section.sh_name;
        const char* name = names + section.sh_name;
        if (std::memchr(name, '\0', nameBytes) == nullptr || std::strcmp(name, "__CCE_KernelArgSize") != 0) {
            continue;
        }
        if (section.sh_size == 0 || section.sh_size % sizeof(uint32_t) != 0 ||
            !IsFileRangeValid(section.sh_offset, section.sh_size, length)) {
            diagnostic = "__CCE_KernelArgSize is malformed";
            return false;
        }
        for (size_t offset = 0; offset < section.sh_size; offset += sizeof(uint32_t)) {
            uint32_t argumentSize = 0;
            if (memcpy_s(
                    &argumentSize, sizeof(argumentSize), bytes + section.sh_offset + offset, sizeof(argumentSize)) !=
                EOK) {
                diagnostic = "cannot read kernel argument size";
                return false;
            }
            maximumArgumentSize = std::max(maximumArgumentSize, argumentSize);
        }
        hasKernelArgumentSize = true;
    }

    const std::string kernelElf(static_cast<const char*>(data), length);
    uint32_t metadataArgumentArea = 0;
    bool hasParameterMetadata = false;
    if (!GetMaximumKernelArgumentArea(kernelElf, metadataArgumentArea, hasParameterMetadata, diagnostic)) {
        diagnostic = "cannot read kernel parameter metadata: " + diagnostic;
        return false;
    }
    if (!hasKernelArgumentSize && !hasParameterMetadata) {
        diagnostic = "__CCE_KernelArgSize and kernel parameter metadata are missing";
        return false;
    }

    maximumArgumentSize = std::max(maximumArgumentSize, metadataArgumentArea);
    constexpr uint32_t MINIMUM_TRACE_ARGUMENT_OFFSET = sizeof(uint64_t);
    maximumArgumentSize = std::max(maximumArgumentSize, MINIMUM_TRACE_ARGUMENT_OFFSET);
    if (maximumArgumentSize > std::numeric_limits<uint32_t>::max() - 7U) {
        diagnostic = "kernel argument area cannot be aligned";
        return false;
    }
    traceArgumentOffset = (maximumArgumentSize + 7U) & ~7U;
    return true;
}

DbiResult RunPipeline(const DbiRequest& request, void*) { return RunDbiPipeline(request); }

bool ReadAll(const boost::filesystem::path& path, std::vector<uint8_t>& data)
{
    std::ifstream input(path.string(), std::ios::binary);
    if (!input) {
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

void Cleanup(const BinaryInstrumentationConfig& config, const std::string& work)
{
    if (!config.keepTemp && !work.empty()) {
        boost::system::error_code error;
        boost::filesystem::remove_all(work, error);
    }
}

void ReportInstrumentationFailure(const BinaryInstrumentationResult& result)
{
    ASCTOOL_ERROR("DBI patch failed at %s: %s", result.stage.c_str(), result.diagnostic.c_str());
}

} // namespace

bool BuildRuntimeInstrumentationConfig(
    const char* socName, const char* runtimeLibrary, uint32_t probeGroupMask, BinaryInstrumentationConfig& config,
    std::string& diagnostic, bool useCompleteProbeSet)
{
    config = {};
    const std::optional<SocVersion> version = ResolveSocVersion(socName);
    if (!version.has_value()) {
        diagnostic = "unsupported Runtime SoC";
        return false;
    }
    switch (*version) {
        case SocVersion::DAV_3510:
            config.arch = "dav-3510";
            break;
    }
    config.toolchainRoot = CannRootFromRuntimeLibrary(runtimeLibrary == nullptr ? "" : runtimeLibrary);
    if (config.toolchainRoot.empty()) {
        diagnostic = "cannot derive CANN root from loaded Runtime library";
        return false;
    }
    config.probeGroups = ProbeGroupsFromMask(probeGroupMask);
    if (config.probeGroups.empty()) {
        diagnostic = "callback selection contains no Probe group";
        return false;
    }
    const std::string root = "/tmp/npu-check-" + std::to_string(static_cast<unsigned long long>(geteuid()));
    config.workDirectory = root + "/requests";
    config.cacheDirectory = root + "/cache";
    if (!EnsurePrivateRuntimeDirectory(root, diagnostic) ||
        !EnsurePrivateRuntimeDirectory(config.workDirectory, diagnostic) ||
        !EnsurePrivateRuntimeDirectory(config.cacheDirectory, diagnostic)) {
        return false;
    }
    config.strict = true;
    config.useCompleteProbeSet = useCompleteProbeSet;
    const char* keepTemp = std::getenv("NPU_CHECK_DBI_KEEP_TEMP");
    config.keepTemp = keepTemp != nullptr && std::strcmp(keepTemp, "1") == 0;
    config.tuneArgs.emplace_back("--append-hbmout-paraminfo");
    return true;
}

BinaryInstrumentationResult InstrumentBinary(
    const BinaryInstrumentationConfig& config, const void* data, size_t length, DbiPipelineRunner runner,
    void* runnerData)
{
    if (data == nullptr || length == 0 || runner == nullptr || config.arch.empty() || config.probeGroups.empty()) {
        return {};
    }

    std::string work;
    try {
        uint32_t traceArgumentOffset = config.traceArgumentOffset;
        uint32_t metadataTraceArgumentOffset = 0;
        std::string diagnostic;
        if (ResolveTraceArgumentOffset(data, length, metadataTraceArgumentOffset, diagnostic)) {
            traceArgumentOffset = std::max(traceArgumentOffset, metadataTraceArgumentOffset);
        } else if (traceArgumentOffset == 0) {
            return {BinaryInstrumentationStatus::Failed, {}, "kernel-arg-offset", diagnostic, 0};
        }
        work = RequestDirectory(config);
        const std::string inputPath = work + "/input.o";
        const std::string outputPath = work + "/patched.o";
        boost::system::error_code error;
        boost::filesystem::create_directories(work, error);
        if (error) {
            Cleanup(config, work);
            return {BinaryInstrumentationStatus::Failed, {}, "work-directory", error.message()};
        }

        std::ofstream input(inputPath, std::ios::binary | std::ios::trunc);
        input.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
        if (!input.good()) {
            Cleanup(config, work);
            return {BinaryInstrumentationStatus::Failed, {}, "write-input", "cannot write input binary"};
        }
        input.close();

        const DbiResult pipeline =
            runner(MakeRequest(config, inputPath, outputPath, work, traceArgumentOffset), runnerData);
        if (!pipeline.success) {
            Cleanup(config, work);
            return {BinaryInstrumentationStatus::Failed, {}, pipeline.stage, pipeline.diagnostic, 0};
        }

        std::vector<uint8_t> patched;
        if (!ReadAll(pipeline.patchedPath, patched)) {
            Cleanup(config, work);
            return {BinaryInstrumentationStatus::Failed, {}, "read-output", "cannot read patched binary", 0};
        }
        Cleanup(config, work);
        return {BinaryInstrumentationStatus::Instrumented, std::move(patched), {}, {}, traceArgumentOffset};
    } catch (const std::exception& error) {
        Cleanup(config, work);
        return {BinaryInstrumentationStatus::Failed, {}, "exception", error.what(), 0};
    } catch (...) {
        Cleanup(config, work);
        return {BinaryInstrumentationStatus::Failed, {}, "exception", "unknown DBI instrumentation failure", 0};
    }
}

BinaryInstrumentationResult InstrumentBinary(const BinaryInstrumentationConfig& config, const void* data, size_t length)
{
    return InstrumentBinary(config, data, length, &RunPipeline, nullptr);
}

RuntimeBinaryInstrumentationResult InstrumentRuntimeBinary(
    const void* data, size_t length, uint32_t probeGroupMask, const char* socName, const char* runtimeLibrary,
    InstrumentedBinaryConsumer consumer, void* consumerData, DbiPipelineRunner runner, void* runnerData,
    uint32_t traceOffset, bool appendParamInfo, bool useCompleteProbeSet) noexcept
{
    constexpr uint32_t strict = 1U;
    try {
        if (probeGroupMask == 0) {
            return {BinaryInstrumentationStatus::Skipped, strict, 0, 0};
        }
        BinaryInstrumentationConfig config;
        std::string diagnostic;
        if (!BuildRuntimeInstrumentationConfig(
                socName, runtimeLibrary, probeGroupMask, config, diagnostic, useCompleteProbeSet)) {
            const BinaryInstrumentationResult failure{
                BinaryInstrumentationStatus::Failed, {}, "runtime-context", diagnostic, 0};
            ReportInstrumentationFailure(failure);
            return {failure.status, strict, 0, 0};
        }
        config.traceArgumentOffset = traceOffset;
        if (!appendParamInfo) {
            config.tuneArgs.clear();
        }
        ASCTOOL_INFO(
            "DBI instrumentation started bytes=%zu probe_groups=%u complete_probe_set=%u", length, probeGroupMask,
            static_cast<unsigned>(useCompleteProbeSet));
        const BinaryInstrumentationResult result = InstrumentBinary(config, data, length, runner, runnerData);
        if (result.status == BinaryInstrumentationStatus::Failed) {
            ReportInstrumentationFailure(result);
            return {result.status, config.strict ? 1U : 0U, 0, 0};
        }
        if (result.status == BinaryInstrumentationStatus::Skipped) {
            return {result.status, config.strict ? 1U : 0U, 0, 0};
        }
        if (consumer == nullptr) {
            const BinaryInstrumentationResult failure{
                BinaryInstrumentationStatus::Failed, {}, "consume-output", "instrumented binary consumer is null", 0};
            ReportInstrumentationFailure(failure);
            return {failure.status, config.strict ? 1U : 0U, 0, 0};
        }
        const int32_t consumerStatus = consumer(result.binary.data(), result.binary.size(), consumerData);
        ASCTOOL_INFO("DBI instrumentation completed bytes=%zu load_result=%d", result.binary.size(), consumerStatus);
        return {result.status, config.strict ? 1U : 0U, consumerStatus, result.traceArgumentOffset};
    } catch (const std::exception& error) {
        const BinaryInstrumentationResult failure{
            BinaryInstrumentationStatus::Failed, {}, "exception", error.what(), 0};
        ReportInstrumentationFailure(failure);
        return {failure.status, strict, 0, 0};
    } catch (...) {
        const BinaryInstrumentationResult failure{
            BinaryInstrumentationStatus::Failed, {}, "exception", "unknown DBI instrumentation failure", 0};
        ReportInstrumentationFailure(failure);
        return {failure.status, strict, 0, 0};
    }
}

RuntimeBinaryInstrumentationResult InstrumentRuntimeBinary(
    const void* data, size_t length, uint32_t probeGroupMask, const char* socName, const char* runtimeLibrary,
    InstrumentedBinaryConsumer consumer, void* consumerData) noexcept
{
    return InstrumentRuntimeBinary(
        data, length, probeGroupMask, socName, runtimeLibrary, consumer, consumerData, &RunPipeline, nullptr);
}

RuntimeBinaryInstrumentationResult InstrumentRuntimeBinaryForEntry(
    const void* data, size_t length, uint32_t probeGroupMask, const char* socName, const char* runtimeLibrary,
    InstrumentedBinaryConsumer consumer, void* consumerData, const uint64_t* entry, uint32_t traceOffset,
    bool appendParamInfo, bool useCompleteProbeSet) noexcept
{
    const auto selectedRunner = [](const DbiRequest& input, void* opaque) {
        DbiRequest request = input;
        if (opaque != nullptr) {
            request.tilingKey = *static_cast<const uint64_t*>(opaque);
        }
        return RunDbiPipeline(request);
    };
    return InstrumentRuntimeBinary(
        data, length, probeGroupMask, socName, runtimeLibrary, consumer, consumerData, selectedRunner,
        const_cast<uint64_t*>(entry), traceOffset, appendParamInfo, useCompleteProbeSet);
}

} // namespace aclsan
