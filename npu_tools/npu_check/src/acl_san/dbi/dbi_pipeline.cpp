/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "dbi/dbi_pipeline.h"

#include "dbi/ctrlbin_generator.h"
#include "dbi/embedded_probe_resources.h"
#include "dbi/kernel_param_metadata.h"
#include "dbi/probe_source_generator.h"
#include "dbi/tool_runner.h"
#include "npu_tool_log.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aclsan {
namespace {
constexpr int HEX_DIGITS_PER_BYTE = 2;
constexpr int HASH_HEX_DIGITS = sizeof(uint64_t) * HEX_DIGITS_PER_BYTE;
constexpr int TOOLCHAIN_PARENT_SEARCH_LIMIT = 3;
constexpr int CANN_PARENT_SEARCH_LIMIT = 5;
constexpr std::size_t OBJDUMP_SYMBOL_NAME_COLUMN = 5;
constexpr std::size_t OBJDUMP_SECTION_COLUMN = 3;
constexpr std::size_t MIN_OBJDUMP_SECTION_FIELDS = OBJDUMP_SECTION_COLUMN + 1;
constexpr std::size_t TEXT_SECTION_PREFIX_BYTES = 6;
constexpr uint64_t FNV1A_OFFSET_BASIS = 1469598103934665603ULL;
constexpr uint64_t FNV1A_PRIME = 1099511628211ULL;
constexpr uint8_t HASH_SEPARATOR = 0xFFU;

constexpr const char* TOOL_NAMES[] = {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"};
std::atomic<uint64_t> g_cacheBuildId{0};

class CacheLock {
public:
    CacheLock() = default;
    ~CacheLock() { Release(); }
    CacheLock(const CacheLock&) = delete;
    CacheLock& operator=(const CacheLock&) = delete;

    void Release()
    {
        if (descriptor_ >= 0) {
            (void)flock(descriptor_, LOCK_UN);
            (void)close(descriptor_);
            descriptor_ = -1;
        }
    }

    bool Acquire(const boost::filesystem::path& path, std::string& diagnostic)
    {
        descriptor_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, (S_IRUSR | S_IWUSR));
        if (descriptor_ < 0) {
            diagnostic = "cannot open cache lock " + path.string() + ": " + std::strerror(errno);
            return false;
        }
        if (flock(descriptor_, LOCK_EX) != 0) {
            diagnostic = "cannot acquire cache lock " + path.string() + ": " + std::strerror(errno);
            (void)close(descriptor_);
            descriptor_ = -1;
            return false;
        }
        return true;
    }

private:
    int descriptor_ = -1;
};

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(boost::filesystem::path path, bool keep = false)
        : path_(std::move(path)), released_(keep)
    {}
    ~TemporaryDirectory()
    {
        if (released_) {
            return;
        }
        boost::system::error_code error;
        boost::filesystem::remove_all(path_, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    void Release() { released_ = true; }

private:
    boost::filesystem::path path_;
    bool released_ = false;
};

class TemporaryFile {
public:
    explicit TemporaryFile(boost::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile()
    {
        if (!released_) {
            boost::system::error_code error;
            boost::filesystem::remove(path_, error);
        }
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    void Release() { released_ = true; }

private:
    boost::filesystem::path path_;
    bool released_ = false;
};

std::string FindInDirectory(const boost::filesystem::path& directory, const char* name)
{
    const auto candidate = directory / name;
    boost::system::error_code error;
    return boost::filesystem::is_regular_file(candidate, error) ? candidate.string() : std::string{};
}

uint64_t HashText(uint64_t hash, const std::string& text)
{
    for (const unsigned char value : text) {
        hash ^= value;
        hash *= FNV1A_PRIME;
    }
    hash ^= HASH_SEPARATOR;
    return hash * FNV1A_PRIME;
}

bool IsNonEmptyFile(const boost::filesystem::path& path)
{
    boost::system::error_code error;
    const auto status = boost::filesystem::symlink_status(path, error);
    return !error && boost::filesystem::is_regular_file(status) && boost::filesystem::file_size(path, error) != 0;
}

bool IsExecutableFile(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    struct stat metadata {};
    if (stat(path.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        (metadata.st_uid != 0 && metadata.st_uid != geteuid()) || (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return false;
    }
    return access(path.c_str(), X_OK) == 0;
}

std::string FileIdentity(const boost::filesystem::path& path)
{
    boost::system::error_code error;
    const auto canonical = boost::filesystem::weakly_canonical(path, error);
    if (error) {
        return path.string();
    }
    const auto size = boost::filesystem::file_size(canonical, error);
    if (error) {
        return canonical.string();
    }
    const auto modified = boost::filesystem::last_write_time(canonical, error);
    if (error) {
        return canonical.string() + ":" + std::to_string(size);
    }
    return canonical.string() + ":" + std::to_string(size) + ":" + std::to_string(modified);
}

boost::filesystem::path ToolchainRoot(const std::string& bisheng)
{
    boost::filesystem::path root = boost::filesystem::path(bisheng).parent_path();
    for (int depth = 0; depth < TOOLCHAIN_PARENT_SEARCH_LIMIT && !root.empty(); ++depth) {
        root = root.parent_path();
    }
    return root;
}

boost::filesystem::path AscendcDevkitRoot(const std::string& bisheng)
{
    const auto root = ToolchainRoot(bisheng);
    for (const char* platform : {"x86_64-linux", "aarch64-linux"}) {
        const auto candidate = root / platform;
        boost::system::error_code error;
        if (boost::filesystem::is_regular_file(candidate / "asc/include/kernel_operator.h", error)) {
            return candidate;
        }
    }
#if defined(__aarch64__)
    return root / "aarch64-linux";
#else
    return root / "x86_64-linux";
#endif
}

std::vector<std::string> ProbeCompileFlags(
    const std::string& arch, const boost::filesystem::path& devkit, ProbeCoreType coreType)
{
    std::vector<std::string> flags{
        "-xcce",
        "-std=c++17",
        "-O2",
        "--cce-aicore-only",
        "--npu-arch=" + arch,
        "-mllvm",
        "-cce-aicore-record-overflow=false",
        "-mllvm",
        "-cce-aicore-record-stack-size=false",
        "-fno-jump-tables",
        "-mllvm",
        "-cce-aicore-merge-function=false",
        "-mllvm",
        "-cce-aicore-addr-transform",
        "-fPIC",
        "-pthread",
        "-DBUILD_DYNAMIC_PROBE",
        "-DTILING_KEY_VAR=0",
        "-I",
        (devkit / "asc").string(),
        "-I",
        (devkit / "asc/include").string(),
        "-I",
        (devkit / "asc/include/basic_api").string(),
        "-I",
        (devkit / "asc/impl/basic_api").string(),
        "-I",
        (devkit / "ascendc/include/highlevel_api").string(),
    };
    if (coreType == ProbeCoreType::Aic) {
        flags.emplace_back("-DNPU_CHECK_PROBE_CUBE_ONLY");
    } else if (coreType == ProbeCoreType::Aiv) {
        flags.emplace_back("-DNPU_CHECK_PROBE_VECTOR_ONLY");
    }
    return flags;
}

std::string TextIdentity(const std::vector<std::string>& values)
{
    uint64_t hash = FNV1A_OFFSET_BASIS;
    for (const std::string& value : values) {
        hash = HashText(hash, value);
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(HASH_HEX_DIGITS) << hash;
    return output.str();
}

bool EnsurePrivateDirectory(const boost::filesystem::path& path, std::string& diagnostic)
{
    boost::system::error_code error;
    boost::filesystem::create_directories(path, error);
    if (error) {
        diagnostic = "cannot create private directory " + path.string() + ": " + error.message();
        return false;
    }
    const auto status = boost::filesystem::symlink_status(path, error);
    if (error || boost::filesystem::is_symlink(status) || !boost::filesystem::is_directory(status)) {
        diagnostic = "private directory is not a real directory: " + path.string();
        return false;
    }
    struct stat metadata {};
    if (lstat(path.c_str(), &metadata) != 0 || metadata.st_uid != geteuid()) {
        diagnostic = "private directory is not owned by the current user: " + path.string();
        return false;
    }
    if (chmod(path.c_str(), (S_IRWXU)) != 0) {
        diagnostic = "cannot restrict private directory " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool WriteExclusiveFile(const boost::filesystem::path& path, const std::string& content)
{
    const int descriptor =
        open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, (S_IRUSR | S_IWUSR));
    if (descriptor < 0) {
        return false;
    }
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t count = write(descriptor, content.data() + written, content.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            (void)close(descriptor);
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return close(descriptor) == 0;
}

bool WriteReplaceFile(const boost::filesystem::path& path, const std::string& content)
{
    const auto temporary =
        path.parent_path() / (".dbi-write-" + std::to_string(static_cast<unsigned long long>(getpid())) + "-" +
                              std::to_string(g_cacheBuildId.fetch_add(1)));
    TemporaryFile cleanup(temporary);
    if (!WriteExclusiveFile(temporary, content)) {
        return false;
    }
    boost::system::error_code error;
    boost::filesystem::rename(temporary, path, error);
    if (error) {
        return false;
    }
    cleanup.Release();
    return true;
}

std::string ReadFile(const boost::filesystem::path& path)
{
    if (!IsNonEmptyFile(path)) {
        return {};
    }
    std::ifstream input(path.string(), std::ios::binary);
    return input ? std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) : std::string{};
}

std::string FileDigest(const boost::filesystem::path& path)
{
    const std::string content = ReadFile(path);
    if (content.empty()) {
        return {};
    }
    uint64_t hash = FNV1A_OFFSET_BASIS;
    for (const unsigned char value : content) {
        hash ^= value;
        hash *= FNV1A_PRIME;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(HASH_HEX_DIGITS) << hash;
    return output.str();
}

std::string GroupIdentity(const std::vector<ProbeGroup>& groups)
{
    std::ostringstream output;
    bool first = true;
    for (const ProbeGroup group : groups) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << ProbeGroupName(group);
    }
    return output.str();
}

std::string ArtifactManifest(
    const boost::filesystem::path& probeObject, const boost::filesystem::path& ctrlBin, const std::string& arch,
    const std::vector<ProbeGroup>& groups, ProbeCoreType coreType, const std::string& objectIdentity)
{
    const std::string probeDigest = FileDigest(probeObject);
    const std::string ctrlDigest = FileDigest(ctrlBin);
    if (probeDigest.empty() || ctrlDigest.empty()) {
        return {};
    }
    std::ostringstream output;
    output << "format=2\narch=" << arch << "\ncore=" << ProbeCoreTypeName(coreType)
           << "\ngroups=" << GroupIdentity(groups) << "\nobjects=" << objectIdentity << "\nprobe=" << probeDigest
           << "\nctrl=" << ctrlDigest << '\n';
    return output.str();
}

bool IsValidCachedArtifact(
    const boost::filesystem::path& directory, const std::string& arch, const std::vector<ProbeGroup>& groups,
    ProbeCoreType coreType, const std::string& objectIdentity)
{
    boost::system::error_code error;
    const auto status = boost::filesystem::symlink_status(directory, error);
    if (error || boost::filesystem::is_symlink(status) || !boost::filesystem::is_directory(status)) {
        return false;
    }
    const auto probeObject = directory / "probe.o";
    const auto ctrlBin = directory / "ctrl.bin";
    const auto manifest = directory / "manifest";
    const std::string expected = ArtifactManifest(probeObject, ctrlBin, arch, groups, coreType, objectIdentity);
    return !expected.empty() && ReadFile(manifest) == expected;
}

std::string ToolFailure(const std::vector<std::string>& arguments, const ToolResult& result)
{
    std::ostringstream output;
    output << arguments.front() << " exited with " << result.exitCode;
    if (!result.standardError.empty()) {
        output << ": " << result.standardError;
    } else if (!result.standardOutput.empty()) {
        output << ": " << result.standardOutput;
    }
    return output.str();
}

// Keep stage and stream context on every line, including chunks of long tool output.
void LogToolOutput(const std::string& stage, const char* stream, const std::string& output)
{
    size_t offset = 0;
    size_t line = 1;
    while (offset < output.size()) {
        const size_t newline = output.find('\n', offset);
        const size_t end = newline == std::string::npos ? output.size() : newline;
        size_t part = 1;
        do {
            const size_t bytes = std::min(size_t{512}, end - offset);
            ASCTOOL_DEBUG(
                "DBI stage=%s output=%s line=%zu part=%zu text=%.*s", stage.c_str(), stream, line, part++,
                static_cast<int>(bytes), output.data() + offset);
            offset += bytes;
        } while (offset < end);
        if (newline == std::string::npos) {
            break;
        }
        offset = newline + 1;
        ++line;
    }
}

bool RunChecked(
    const std::string& stage, const std::vector<std::string>& arguments, DbiResult& result,
    std::string* standardOutput = nullptr)
{
    std::ostringstream command;
    for (const auto& argument : arguments) {
        command << std::quoted(argument) << ' ';
    }
    LogToolOutput(stage, "command", command.str());
    const ToolResult toolResult = RunTool(arguments);
    ASCTOOL_DEBUG("DBI stage=%s exit=%d", stage.c_str(), toolResult.exitCode);
    LogToolOutput(stage, "stdout", toolResult.standardOutput);
    LogToolOutput(stage, "stderr", toolResult.standardError);
    if (standardOutput != nullptr) {
        *standardOutput = toolResult.standardOutput;
    }
    if (toolResult.exitCode == 0) {
        return true;
    }
    result.stage = stage;
    result.diagnostic = ToolFailure(arguments, toolResult);
    return false;
}

std::vector<std::string> ParseTextSymbols(const std::string& text, const std::string& property)
{
    std::vector<std::string> symbols;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::vector<std::string> items{
            std::istream_iterator<std::string>(fields), std::istream_iterator<std::string>()};
        const bool textSection = items.size() >= MIN_OBJDUMP_SECTION_FIELDS &&
                                 (items[OBJDUMP_SECTION_COLUMN] == ".text" ||
                                  items[OBJDUMP_SECTION_COLUMN].compare(0, TEXT_SECTION_PREFIX_BYTES, ".text.") == 0);
        if (items.size() > OBJDUMP_SYMBOL_NAME_COLUMN && items[1] == property && items[2] == "F" && textSection) {
            symbols.push_back(items[OBJDUMP_SYMBOL_NAME_COLUMN]);
        }
    }
    return symbols;
}

bool EndsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string SelectKernelTextSymbol(const std::string& text, const std::optional<uint64_t>& tilingKey)
{
    const auto symbols = ParseTextSymbols(text, "g");
    if (!tilingKey.has_value()) {
        return symbols.empty() ? std::string{} : symbols.front();
    }

    const std::string key = "_" + std::to_string(*tilingKey);
    // ACLNN Mix kernels publish one symbol per tiling key and core half. Match the
    // selected entry exactly and use the AIC half as the ordering anchor, as MS does.
    for (const char* suffix : {"_mix_aic", "_aic", "", "_mix_aiv", "_aiv"}) {
        const std::string expected = key + suffix;
        const auto selected = std::find_if(
            symbols.begin(), symbols.end(), [&](const std::string& symbol) { return EndsWith(symbol, expected); });
        if (selected != symbols.end()) {
            return *selected;
        }
    }
    return {};
}

bool HasTextSymbol(const std::string& text, const std::string& property, const std::string& symbol)
{
    const std::string mangledPrefix = "_Z" + std::to_string(symbol.size()) + symbol;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        const std::vector<std::string> items{
            std::istream_iterator<std::string>(fields), std::istream_iterator<std::string>()};
        const bool textSection = items.size() >= MIN_OBJDUMP_SECTION_FIELDS &&
                                 (items[OBJDUMP_SECTION_COLUMN] == ".text" ||
                                  items[OBJDUMP_SECTION_COLUMN].compare(0, TEXT_SECTION_PREFIX_BYTES, ".text.") == 0);
        if (items.size() > OBJDUMP_SYMBOL_NAME_COLUMN && items[1] == property && items[2] == "F" && textSection &&
            (items[OBJDUMP_SYMBOL_NAME_COLUMN] == symbol ||
             items[OBJDUMP_SYMBOL_NAME_COLUMN].find(mangledPrefix) != std::string::npos)) {
            return true;
        }
    }
    return false;
}

struct GroupArtifact {
    boost::filesystem::path object;
    std::string identity;
};

std::string GroupManifest(
    const boost::filesystem::path& object, const std::string& arch, ProbeCoreType coreType, ProbeGroup group,
    const GeneratedProbeSource& generated, const std::string& compilerIdentity)
{
    const std::string digest = FileDigest(object);
    if (digest.empty()) {
        return {};
    }
    std::ostringstream output;
    output << "format=2\narch=" << arch << "\ncore=" << ProbeCoreTypeName(coreType)
           << "\ngroup=" << ProbeGroupName(group) << "\nsource=" << generated.identity
           << "\ngenerator=" << ProbeGeneratorIdentity() << "\nresources=" << EmbeddedProbeResourceIdentity()
           << "\ncompiler=" << compilerIdentity << "\nobject=" << digest << '\n';
    return output.str();
}

bool IsValidGroupArtifact(
    const boost::filesystem::path& directory, const std::string& arch, ProbeCoreType coreType, ProbeGroup group,
    const GeneratedProbeSource& generated, const std::string& compilerIdentity)
{
    boost::system::error_code error;
    const auto status = boost::filesystem::symlink_status(directory, error);
    if (error || boost::filesystem::is_symlink(status) || !boost::filesystem::is_directory(status)) {
        return false;
    }
    const auto object = directory / "group.o";
    const std::string expected = GroupManifest(object, arch, coreType, group, generated, compilerIdentity);
    return !expected.empty() && ReadFile(directory / "manifest") == expected;
}

bool ReplaceDirectory(
    const boost::filesystem::path& staging, const boost::filesystem::path& destination, DbiResult& result,
    const std::string& stage)
{
    boost::system::error_code error;
    const auto oldStatus = boost::filesystem::symlink_status(destination, error);
    if (!error && boost::filesystem::exists(oldStatus)) {
        boost::filesystem::remove_all(destination, error);
        if (error) {
            result.stage = stage;
            result.diagnostic = "cannot replace invalid artifact: " + error.message();
            return false;
        }
    }
    error.clear();
    boost::filesystem::rename(staging, destination, error);
    if (error) {
        result.stage = stage;
        result.diagnostic = "cannot publish artifact: " + error.message();
        return false;
    }
    return true;
}

bool GetOrBuildGroupArtifact(
    const DbiRequest& request, const ToolchainPaths& tools, const boost::filesystem::path& groupsRoot, ProbeGroup group,
    DbiResult& result, GroupArtifact& artifact)
{
    const GeneratedProbeSource generated = GenerateProbeSource(request.arch, group);
    if (!generated.success) {
        result.stage = "render-probe";
        result.diagnostic = generated.diagnostic;
        return false;
    }
    const auto devkit = AscendcDevkitRoot(tools.bisheng);
    const auto compilerFlags = ProbeCompileFlags(request.arch, devkit, request.coreType);
    const std::string compilerIdentity = FileIdentity(tools.bisheng) + ":" + TextIdentity(compilerFlags) + ":" +
                                         std::string(EmbeddedProbeResourceIdentity());
    const std::string cacheKey = MakeCacheKey(
        request.arch, {group}, generated.identity + ":" + ProbeGeneratorIdentity() + ":" + compilerIdentity,
        request.coreType);
    const auto directory = groupsRoot / cacheKey;
    CacheLock lock;
    if (!lock.Acquire(groupsRoot / (cacheKey + ".lock"), result.diagnostic)) {
        result.stage = "group-cache-lock";
        return false;
    }
    if (!IsValidGroupArtifact(directory, request.arch, request.coreType, group, generated, compilerIdentity)) {
        const std::string buildId = std::to_string(static_cast<unsigned long long>(getpid())) + "-" +
                                    std::to_string(g_cacheBuildId.fetch_add(1));
        const auto staging = groupsRoot / (".build-" + cacheKey + "-" + buildId);
        boost::system::error_code error;
        if (!boost::filesystem::create_directory(staging, error) || error ||
            !EnsurePrivateDirectory(staging, result.diagnostic)) {
            result.stage = "group-cache-directory";
            if (result.diagnostic.empty()) {
                result.diagnostic = "cannot create group staging directory: " + error.message();
            }
            return false;
        }
        TemporaryDirectory stagingCleanup(staging, request.keepTemp);
        const auto includeDirectory = staging / "include";
        const auto sourceDirectory = staging / "src";
        if (!EnsurePrivateDirectory(includeDirectory, result.diagnostic) ||
            !EnsurePrivateDirectory(sourceDirectory, result.diagnostic)) {
            result.stage = "materialize-source";
            return false;
        }
        const auto source = sourceDirectory / ("generated-" + ProbeGroupName(group) + ".cpp");
        if (!WriteExclusiveFile(includeDirectory / "trace_record.h", std::string(EmbeddedTraceRecordHeader())) ||
            !WriteExclusiveFile(includeDirectory / "trace_buffer_abi.h", std::string(EmbeddedTraceBufferAbiHeader())) ||
            !WriteExclusiveFile(source, generated.source) ||
            !WriteExclusiveFile(sourceDirectory / (ProbeGroupName(group) + ".map"), generated.sourceMap)) {
            result.stage = "materialize-source";
            result.diagnostic = "cannot materialize generated Probe source for " + ProbeGroupName(group);
            return false;
        }

        const auto stagedObject = staging / "group.o";
        std::vector<std::string> arguments{tools.bisheng};
        arguments.insert(arguments.end(), compilerFlags.begin(), compilerFlags.end());
        arguments.insert(
            arguments.end(), {"-I", includeDirectory.string(), "-c", source.string(), "-o", stagedObject.string()});
        if (!RunChecked("compile-probe", arguments, result) || !IsNonEmptyFile(stagedObject)) {
            if (result.diagnostic.empty()) {
                result.stage = "compile-probe";
                result.diagnostic = "Bisheng did not create " + stagedObject.string();
            }
            return false;
        }
        std::string symbols;
        if (!RunChecked("validate-probe", {tools.llvmObjdump, "--syms", stagedObject.string()}, result, &symbols)) {
            return false;
        }
        const auto missing = std::find_if(generated.symbols.begin(), generated.symbols.end(), [&](const auto& symbol) {
            return !HasTextSymbol(symbols, "w", symbol);
        });
        if (missing != generated.symbols.end()) {
            if (result.diagnostic.empty()) {
                result.stage = "validate-probe";
                result.diagnostic = "generated Probe group is missing weak text symbol " + *missing;
            }
            return false;
        }
        const std::string manifest =
            GroupManifest(stagedObject, request.arch, request.coreType, group, generated, compilerIdentity);
        if (manifest.empty() || !WriteExclusiveFile(staging / "manifest", manifest)) {
            result.stage = "publish-group-cache";
            result.diagnostic = "cannot create group manifest";
            return false;
        }
        if (!ReplaceDirectory(staging, directory, result, "publish-group-cache")) {
            return false;
        }
        stagingCleanup.Release();
        if (!IsValidGroupArtifact(directory, request.arch, request.coreType, group, generated, compilerIdentity)) {
            result.stage = "publish-group-cache";
            result.diagnostic = "published group artifact failed validation";
            return false;
        }
    }
    artifact.object = directory / "group.o";
    artifact.identity = cacheKey + ":" + FileDigest(artifact.object);
    return true;
}

} // namespace

std::string ProbeCoreTypeName(ProbeCoreType coreType)
{
    switch (coreType) {
        case ProbeCoreType::Aic:
            return "aic";
        case ProbeCoreType::Aiv:
            return "aiv";
        case ProbeCoreType::Mix:
            return "mix";
        case ProbeCoreType::Unknown:
            return "unknown";
    }
    return "unknown";
}

ProbeCoreType DetectProbeCoreType(const std::string& kernelSymbols, const std::string& selectedKernelSymbol)
{
    if (selectedKernelSymbol.find("_mix_aic") != std::string::npos ||
        selectedKernelSymbol.find("_mix_aiv") != std::string::npos) {
        return ProbeCoreType::Mix;
    }
    if (EndsWith(selectedKernelSymbol, "_aic")) {
        return ProbeCoreType::Aic;
    }
    if (EndsWith(selectedKernelSymbol, "_aiv")) {
        return ProbeCoreType::Aiv;
    }

    const bool hasCubePipe = kernelSymbols.find("g_cubeTPipePtr") != std::string::npos;
    const bool hasVectorPipe = kernelSymbols.find("g_vecTPipePtr") != std::string::npos;
    if (hasCubePipe != hasVectorPipe) {
        return hasCubePipe ? ProbeCoreType::Aic : ProbeCoreType::Aiv;
    }
    return ProbeCoreType::Mix;
}

std::vector<ProbeGroup> ResolveProbeGroups(
    const std::vector<ProbeGroup>& requestedGroups, ProbeCoreType coreType, bool useCompleteProbeSet)
{
    // ACLNN Mix kernels contain both the Cube and Vector halves.  MS builds
    // the control table from the complete probe catalog for this path; using
    // only the active memory groups leaves scalar/control callsites absent
    // from the selected tiling-key variant.
    if (useCompleteProbeSet && (coreType == ProbeCoreType::Aic || coreType == ProbeCoreType::Mix)) {
        return AllProbeGroups();
    }
    return NormalizeProbeGroups(requestedGroups);
}

bool ToolchainPaths::Complete() const
{
    return IsExecutableFile(bisheng) && IsExecutableFile(bishengTune) && IsExecutableFile(ldLld) &&
           IsExecutableFile(llvmObjdump);
}

std::string ValidateRequest(const DbiRequest& request)
{
    if (request.inputKernel.empty()) {
        return "input kernel is empty";
    }
    if (request.outputKernel.empty()) {
        return "output kernel is empty";
    }
    if (request.arch.empty()) {
        return "architecture is empty";
    }
    if (NormalizeProbeGroups(request.probeGroups).empty()) {
        return "probe set is empty";
    }
    return {};
}

ToolchainPaths ResolveToolchain(const std::string& cannRoot)
{
    if (cannRoot.empty()) {
        return {};
    }
    const auto directory = boost::filesystem::path(cannRoot) / "tools/bisheng_compiler/bin";
    return {
        FindInDirectory(directory, TOOL_NAMES[0]), FindInDirectory(directory, TOOL_NAMES[1]),
        FindInDirectory(directory, TOOL_NAMES[2]), FindInDirectory(directory, TOOL_NAMES[3])};
}

std::string CannRootFromRuntimeLibrary(const std::string& runtimeLibrary)
{
    if (runtimeLibrary.empty()) {
        return {};
    }
    boost::system::error_code error;
    const auto library = boost::filesystem::weakly_canonical(runtimeLibrary, error);
    if (error || !boost::filesystem::is_regular_file(library, error)) {
        return {};
    }
    auto candidate = library.parent_path();
    for (int depth = 0; depth < CANN_PARENT_SEARCH_LIMIT && !candidate.empty(); ++depth) {
        const auto tools = candidate / "tools/bisheng_compiler/bin";
        bool complete = true;
        for (const char* name : TOOL_NAMES) {
            if (!boost::filesystem::is_regular_file(tools / name, error)) {
                complete = false;
                break;
            }
        }
        if (complete) {
            return candidate.string();
        }
        const auto parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return {};
}

std::string MakeCacheKey(
    const std::string& arch, const std::vector<ProbeGroup>& groups, const std::string& objectIdentity,
    ProbeCoreType coreType)
{
    uint64_t hash = FNV1A_OFFSET_BASIS;
    hash = HashText(hash, arch);
    hash = HashText(hash, ProbeCoreTypeName(coreType));
    for (const auto group : NormalizeProbeGroups(groups)) {
        hash = HashText(hash, ProbeGroupName(group));
    }
    hash = HashText(hash, objectIdentity);
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(HASH_HEX_DIGITS) << hash;
    return output.str();
}

DbiResult RunDbiPipeline(const DbiRequest& request)
{
    // 校验调用方传入的参数，并快照本次请求对应的 ProbeGroup 集合。
    DbiResult result{};
    result.traceArgumentOffset = request.traceArgumentOffset;
    result.stage = "validate";
    result.diagnostic = ValidateRequest(request);
    if (!result.diagnostic.empty()) {
        return result;
    }
    const auto requestedGroups = NormalizeProbeGroups(request.probeGroups);
    if (!GenerateProbeSource(request.arch, requestedGroups.front()).success) {
        result.stage = "architecture";
        result.diagnostic = "unsupported Probe architecture " + request.arch;
        return result;
    }

    // Probe 源码在 load hook 中生成，因此运行时需要同一 CANN root 下的完整 Bisheng 工具链。
    const ToolchainPaths tools = ResolveToolchain(request.toolchainRoot);
    if (!tools.Complete()) {
        result.stage = "toolchain";
        result.diagnostic = "DBI runtime toolchain is incomplete: bisheng=" + tools.bisheng +
                            " bisheng-tune=" + tools.bishengTune + " ld.lld=" + tools.ldLld +
                            " llvm-objdump=" + tools.llvmObjdump;
        return result;
    }

    std::string kernelSymbols;
    if (!RunChecked("inspect-kernel", {tools.llvmObjdump, "--syms", request.inputKernel}, result, &kernelSymbols)) {
        return result;
    }
    const std::string selectedKernelSymbol = SelectKernelTextSymbol(kernelSymbols, request.tilingKey);
    if (selectedKernelSymbol.empty()) {
        result.stage = "symbol-ordering";
        result.diagnostic = request.tilingKey.has_value() ? "cannot identify kernel text symbol for tiling key " +
                                                                std::to_string(*request.tilingKey) :
                                                            "cannot identify kernel text symbol";
        return result;
    }
    // The launch tiling key selects the executable entry for Tune and core-type detection. The linker ordering file
    // follows the original ELF layout and must remain anchored at its first kernel entry for multi-entry Mix kernels.
    const std::string orderingKernelSymbol = SelectKernelTextSymbol(kernelSymbols, std::nullopt);
    if (orderingKernelSymbol.empty()) {
        result.stage = "symbol-ordering";
        result.diagnostic = "cannot identify kernel ordering anchor";
        return result;
    }
    DbiRequest resolvedRequest = request;
    if (resolvedRequest.coreType == ProbeCoreType::Unknown) {
        resolvedRequest.coreType = DetectProbeCoreType(kernelSymbols, selectedKernelSymbol);
    }
    const auto groups =
        ResolveProbeGroups(requestedGroups, resolvedRequest.coreType, resolvedRequest.useCompleteProbeSet);
    const std::string tilingKeyText = request.tilingKey.has_value() ? std::to_string(*request.tilingKey) : "none";
    LogToolOutput(
        "kernel-selection", "selection",
        "kernel=" + selectedKernelSymbol + " ordering_anchor=" + orderingKernelSymbol + " tiling_key=" + tilingKeyText +
            " core_type=" + ProbeCoreTypeName(resolvedRequest.coreType) +
            " complete_probe_set=" + (groups == AllProbeGroups() ? "1" : "0"));

    // 准备本次流水线使用的工作目录和跨请求复用的缓存目录。
    boost::system::error_code error;
    boost::filesystem::create_directories(request.workDirectory, error);
    if (error) {
        result.stage = "work-directory";
        result.diagnostic = error.message();
        return result;
    }
    if (!EnsurePrivateDirectory(request.cacheDirectory, result.diagnostic)) {
        result.stage = "cache-directory";
        return result;
    }
    const auto groupsRoot = boost::filesystem::path(request.cacheDirectory) / "groups";
    const auto aggregatesRoot = boost::filesystem::path(request.cacheDirectory) / "aggregates";
    if (!EnsurePrivateDirectory(groupsRoot, result.diagnostic) ||
        !EnsurePrivateDirectory(aggregatesRoot, result.diagnostic)) {
        result.stage = "cache-directory";
        return result;
    }

    // 每组源码和对象独立缓存，Domain ID 的不同组合可以复用相同 group.o。
    std::vector<std::string> objectPaths;
    std::string groupIdentity;
    for (const ProbeGroup group : groups) {
        GroupArtifact artifact;
        if (!GetOrBuildGroupArtifact(resolvedRequest, tools, groupsRoot, group, result, artifact)) {
            return result;
        }
        groupIdentity.append(ProbeGroupName(group)).append(":").append(artifact.identity).append("\n");
        objectPaths.push_back(artifact.object.string());
    }

    // 聚合缓存把同一 normalized groups 的 probe.o 与 ctrl.bin 作为一个不可分割的产物发布。
    const std::string artifactIdentity = groupIdentity + ":probe-link-aicorelinux-v1:" + CtrlBinGeneratorIdentity() +
                                         ":" + std::string(EmbeddedCtrlBinImplementationIdentity()) + ":" +
                                         FileIdentity(tools.ldLld);
    const std::string cacheKey = MakeCacheKey(request.arch, groups, artifactIdentity, resolvedRequest.coreType);
    const auto artifactDirectory = aggregatesRoot / cacheKey;
    CacheLock cacheLock;
    if (!cacheLock.Acquire(aggregatesRoot / (cacheKey + ".lock"), result.diagnostic)) {
        result.stage = "cache-lock";
        return result;
    }
    const auto probeObject = artifactDirectory / "probe.o";
    const auto ctrlBin = artifactDirectory / "ctrl.bin";
    if (!IsValidCachedArtifact(artifactDirectory, request.arch, groups, resolvedRequest.coreType, artifactIdentity)) {
        const std::string buildId = std::to_string(static_cast<unsigned long long>(getpid())) + "-" +
                                    std::to_string(g_cacheBuildId.fetch_add(1));
        const auto stagingDirectory = aggregatesRoot / (".build-" + cacheKey + "-" + buildId);
        error.clear();
        if (!boost::filesystem::create_directory(stagingDirectory, error) || error ||
            !EnsurePrivateDirectory(stagingDirectory, result.diagnostic)) {
            result.stage = "cache-directory";
            if (result.diagnostic.empty()) {
                result.diagnostic = "cannot create Probe artifact staging directory: " + error.message();
            }
            return result;
        }
        TemporaryDirectory stagingCleanup(stagingDirectory, request.keepTemp);

        // 将已校验的 group objects 合并，保证后续只需向内核链接一个 probe.o。
        const auto stagedProbeObject = stagingDirectory / "probe.o";
        std::vector<std::string> linkArguments{tools.ldLld, "-r", "-m", "aicorelinux"};
        linkArguments.insert(linkArguments.end(), objectPaths.begin(), objectPaths.end());
        linkArguments.insert(linkArguments.end(), {"-o", stagedProbeObject.string()});
        if (!RunChecked("link-probe", linkArguments, result) || !IsNonEmptyFile(stagedProbeObject)) {
            if (result.diagnostic.empty()) {
                result.stage = "link-probe";
                result.diagnostic = "ld.lld did not create " + stagedProbeObject.string();
            }
            return result;
        }

        // 使用同一组 groups 生成 ctrl.bin，并与 probe.o 作为一个目录原子发布。
        const auto stagedCtrlBin = stagingDirectory / "ctrl.bin";
        if (!GenerateCtrlBin(stagedCtrlBin.string(), groups, result.diagnostic)) {
            result.stage = "generate-ctrlbin";
            return result;
        }
        const std::string manifest = ArtifactManifest(
            stagedProbeObject, stagedCtrlBin, request.arch, groups, resolvedRequest.coreType, artifactIdentity);
        if (manifest.empty() || !WriteExclusiveFile(stagingDirectory / "manifest", manifest)) {
            result.stage = "publish-cache";
            result.diagnostic = "cannot create Probe artifact manifest";
            return result;
        }

        if (!ReplaceDirectory(stagingDirectory, artifactDirectory, result, "publish-cache")) {
            return result;
        }
        stagingCleanup.Release();
        if (!IsValidCachedArtifact(
                artifactDirectory, request.arch, groups, resolvedRequest.coreType, artifactIdentity)) {
            result.stage = "publish-cache";
            result.diagnostic = "published Probe artifact failed validation";
            return result;
        }
    }
    // 缓存文件发布后不再修改，后续均为请求私有操作，因此提前释放缓存锁。
    cacheLock.Release();

    // 读取探针对象的符号表，提取待合并的探针弱符号。
    std::string probeSymbols;
    if (!RunChecked("inspect-probe", {tools.llvmObjdump, "--syms", probeObject.string()}, result, &probeSymbols)) {
        return result;
    }
    const auto probeTextSymbols = ParseTextSymbols(probeSymbols, "w");
    const std::string probeSymbol = probeTextSymbols.empty() ? std::string{} : probeTextSymbols.front();
    if (probeSymbol.empty()) {
        result.stage = "symbol-ordering";
        result.diagnostic = "cannot identify Probe text symbol";
        return result;
    }
    LogToolOutput(
        "symbol-selection", "selection",
        "kernel=" + selectedKernelSymbol + " ordering_anchor=" + orderingKernelSymbol + " probe=" + probeSymbol +
            " tiling_key=" + tilingKeyText + " core_type=" + ProbeCoreTypeName(resolvedRequest.coreType));
    // 生成符号排序文件，确保链接后原始内核正文位于探针入口之前。
    const auto orderingFile = boost::filesystem::path(request.workDirectory) / "symbol_ordering.txt";
    if (!WriteReplaceFile(orderingFile, orderingKernelSymbol + "\n" + probeSymbol)) {
        result.stage = "symbol-ordering";
        result.diagnostic = "cannot write " + orderingFile.string();
        return result;
    }
    // 按指定符号顺序将 probe.o 与原始内核链接，生成待插桩的中间内核对象。
    const auto requestBuildId = std::to_string(g_cacheBuildId.fetch_add(1));
    const auto mergedKernel = boost::filesystem::path(request.workDirectory) / "kernel_with_probe.o";
    const auto stagedMergedKernel =
        boost::filesystem::path(request.workDirectory) / (".kernel_with_probe-" + requestBuildId);
    TemporaryFile mergedCleanup(stagedMergedKernel);
    const std::vector<std::string> mergeArguments{
        tools.ldLld,
        "-m",
        "aicorelinux",
        "-Ttext=0",
        "-execute-probe",
        "--symbol-ordering-file",
        orderingFile.string(),
        "-z",
        "separate-loadable-segments",
        probeObject.string(),
        request.inputKernel,
        "-static",
        "-q",
        "-o",
        stagedMergedKernel.string()};
    if (!RunChecked("merge-kernel", mergeArguments, result) || !IsNonEmptyFile(stagedMergedKernel)) {
        if (result.diagnostic.empty()) {
            result.stage = "merge-kernel";
            result.diagnostic = "ld.lld did not create " + stagedMergedKernel.string();
        }
        return result;
    }
    boost::filesystem::rename(stagedMergedKernel, mergedKernel, error);
    if (error) {
        result.stage = "publish-kernel";
        result.diagnostic = "cannot publish " + mergedKernel.string() + ": " + error.message();
        return result;
    }
    mergedCleanup.Release();

    // 使用 bisheng-tune 和 ctrl.bin 对合并后的内核执行 DBI 插桩。
    // 先输出到目标文件的临时同级文件，避免工具中途失败时暴露不完整的内核。
    const auto stagedOutput =
        boost::filesystem::path(request.outputKernel).parent_path() /
        (".dbi-patched-" + std::to_string(static_cast<unsigned long long>(getpid())) + "-" + requestBuildId);
    TemporaryFile outputCleanup(stagedOutput);
    std::vector<std::string> tuneArguments{
        tools.bishengTune,
        "--action=instru-probe",
        "--tune-argsize=" + std::to_string(request.traceArgumentOffset),
        "--instru-memprobe",
        mergedKernel.string(),
        "--dbi-config=" + ctrlBin.string(),
        "-o=" + stagedOutput.string()};
    tuneArguments.insert(tuneArguments.end(), request.extraTuneArgs.begin(), request.extraTuneArgs.end());
    if (request.tilingKey.has_value()) {
        for (const auto& argument : request.extraTuneArgs) {
            if (argument.rfind("--tiling-key", 0) == 0 || argument.rfind("--tune-argsize", 0) == 0) {
                result.stage = "selection";
                result.diagnostic = "extra tune arguments override the selected entry or trace offset";
                return result;
            }
        }
        tuneArguments.emplace_back("--tiling-key=" + std::to_string(*request.tilingKey));
    }
    if (!RunChecked("bisheng-tune", tuneArguments, result) || !IsNonEmptyFile(stagedOutput)) {
        if (result.diagnostic.empty()) {
            result.stage = "bisheng-tune";
            result.diagnostic = "bisheng-tune did not create " + stagedOutput.string();
        }
        return result;
    }
    // 调用ArgsArray场景会传入--append-hbmout-paraminfo参数。
    // 此时还需要额外修改插桩后.o的meta段使得与DBI的统一偏移保持一致
    if (std::find(request.extraTuneArgs.begin(), request.extraTuneArgs.end(), "--append-hbmout-paraminfo") !=
        request.extraTuneArgs.end()) {
        result.stage = "kernel-param-metadata";
        std::string patched = ReadFile(stagedOutput);
        std::string kernelBeforePatch = ReadFile(request.inputKernel);
        // 获取 整改插桩.o的meta段后的内容
        if (!ModifyKernelParamMetadata(
                kernelBeforePatch, patched, request.traceArgumentOffset, result.diagnostic,
                request.tilingKey ? &*request.tilingKey : nullptr)) {
            result.diagnostic = "cannot modify kernel ELF metadata: " + result.diagnostic;
            return result;
        }
        if (!WriteReplaceFile(stagedOutput, patched)) {
            result.diagnostic = "cannot replace patched kernel with modified metadata";
            return result;
        }
    }
    // 插桩成功后原子发布最终内核，并填写流水线成功结果。
    boost::filesystem::rename(stagedOutput, request.outputKernel, error);
    if (error) {
        result.stage = "publish-kernel";
        result.diagnostic = "cannot publish " + request.outputKernel + ": " + error.message();
        return result;
    }
    outputCleanup.Release();
    result.success = true;
    result.patchedPath = request.outputKernel;
    result.stage = "complete";
    result.diagnostic.clear();
    return result;
}

} // namespace aclsan
