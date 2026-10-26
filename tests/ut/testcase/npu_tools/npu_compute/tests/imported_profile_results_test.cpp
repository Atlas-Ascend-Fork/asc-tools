/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "import/imported_profile_results.h"
#include "import/import_output_directory.h"

#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "report/collection_processor.h"
#include "report/rep_encoder.h"

namespace {

using npucompute::cli::EncodeRep;
using npucompute::cli::ImportedProfileEntry;
using npucompute::cli::NpuRepFileType;
using npucompute::cli::PackDirectoryToRep;
using npucompute::cli::ReadImportedProfileResults;
using npucompute::cli::UnpackImportedProfileResults;

class TempDirectory {
public:
    TempDirectory()
    {
        std::string path_template =
            (boost::filesystem::temp_directory_path() / "npu-compute-import-test-XXXXXX").string();
        path_template.push_back('\0');
        char* created = ::mkdtemp(path_template.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TempDirectory()
    {
        if (!path_.empty()) {
            boost::system::error_code error;
            boost::filesystem::remove_all(path_, error);
        }
    }

    const boost::filesystem::path& Path() const { return path_; }

private:
    boost::filesystem::path path_;
};

std::vector<uint8_t> Bytes(std::string_view value) { return {value.begin(), value.end()}; }

bool WriteFile(const boost::filesystem::path& path, const std::vector<uint8_t>& content)
{
    std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    return output.good();
}

bool ReadFile(const boost::filesystem::path& path, std::vector<uint8_t>* content)
{
    std::ifstream input(path.string(), std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    content->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

bool BuildNestedRep(std::vector<uint8_t>* encoded)
{
    std::string error;
    std::vector<uint8_t> child;
    if (!EncodeRep({{"Memory.csv", NpuRepFileType::Csv, Bytes("name,value\nmemory,2\n")}}, &child, &error)) {
        return false;
    }
    return EncodeRep(
        {{"HardwareInfo.jsonl", NpuRepFileType::Jsonl, Bytes("{\"category\":\"Host Info\"}\n")},
         {"device_0.npu.rep", NpuRepFileType::NpuRep, child}},
        encoded, &error);
}

const std::vector<uint8_t>& HardwareInfoBytes()
{
    static const std::vector<uint8_t> content = Bytes("{\"category\":\"Host Info\"}\n"
                                                      "{\"category\":\"Device Info\"}\n"
                                                      "{\"category\":\"CPU Information\"}\n"
                                                      "{\"category\":\"AI Core Information\"}\n"
                                                      "{\"category\":\"Memory Information\"}\n");
    return content;
}

bool BuildRecursiveFixture(const boost::filesystem::path& fixture, std::vector<uint8_t>* encoded)
{
    if (!boost::filesystem::create_directories(fixture / "device_0" / "details") ||
        !WriteFile(fixture / "HardwareInfo.jsonl", HardwareInfoBytes()) ||
        !WriteFile(fixture / "metadata.json", Bytes("{\"version\":1}\n")) ||
        !WriteFile(fixture / "profile.db", {0x53U, 0x51U, 0x4cU, 0x00U, 0xffU}) ||
        !WriteFile(fixture / "trace.txt", Bytes("instruction text\n")) ||
        !WriteFile(fixture / "device_0" / "Memory.csv", Bytes("name,value\nmemory,2\n")) ||
        !WriteFile(fixture / "device_0" / "details" / "L2Cache.csv", Bytes("name,value\nl2,3\n"))) {
        return false;
    }
    std::string error;
    return PackDirectoryToRep(fixture, encoded, &error);
}

std::vector<std::string> RelativeDirectoryEntries(const boost::filesystem::path& root)
{
    std::vector<std::string> entries;
    for (const boost::filesystem::directory_entry& entry : boost::filesystem::recursive_directory_iterator(root)) {
        std::string relative = boost::filesystem::relative(entry.path(), root).generic_string();
        if (entry.is_directory()) {
            relative += "/";
        }
        entries.push_back(std::move(relative));
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

bool SameDirectoryTrees(const boost::filesystem::path& left, const boost::filesystem::path& right)
{
    if (RelativeDirectoryEntries(left) != RelativeDirectoryEntries(right)) {
        return false;
    }
    for (const boost::filesystem::directory_entry& entry : boost::filesystem::recursive_directory_iterator(left)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::vector<uint8_t> leftContent;
        std::vector<uint8_t> rightContent;
        if (!ReadFile(entry.path(), &leftContent) ||
            !ReadFile(right / boost::filesystem::relative(entry.path(), left), &rightContent) ||
            leftContent != rightContent) {
            return false;
        }
    }
    return true;
}

TEST(NpuComputeImportedProfileResults, EnforcesReportDepthLimits)
{
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    const boost::filesystem::path input = temporary.Path() / "depth.npu-rep";
    std::string error;
    for (std::size_t depth : {1U, 3U, 64U, 65U, 5000U}) {
        std::vector<uint8_t> encoded;
        ASSERT_TRUE(EncodeRep({{"value.json", NpuRepFileType::Json, Bytes("{}")}}, &encoded, &error));
        for (std::size_t level = 1; level < depth; ++level) {
            std::vector<uint8_t> parent;
            ASSERT_TRUE(EncodeRep({{"d.npu.rep", NpuRepFileType::NpuRep, encoded}}, &parent, &error));
            encoded = std::move(parent);
        }
        ASSERT_TRUE(WriteFile(input, encoded));
        std::vector<ImportedProfileEntry> results(1);
        const bool read = ReadImportedProfileResults(input, &results, &error);
        ASSERT_TRUE(read == (depth <= 64U));
        if (read) {
            const auto* entries = &results;
            for (std::size_t level = 1; level < depth; ++level) {
                ASSERT_TRUE(entries->size() == 1U);
                entries = &entries->front().children;
            }
            ASSERT_TRUE(entries->size() == 1U);
            ASSERT_TRUE(entries->front().payload == Bytes("{}"));
        } else {
            ASSERT_TRUE(results.empty());
            ASSERT_TRUE(error.find("maximum nested report depth exceeded") != std::string::npos);
        }
    }

    const boost::filesystem::path source = temporary.Path() / "source";
    const boost::filesystem::path output = temporary.Path() / "output";
    ASSERT_TRUE(boost::filesystem::create_directory(source));
    ASSERT_TRUE(boost::filesystem::create_directory(output));
    boost::filesystem::path deepest = source;
    for (std::size_t level = 1; level < 64U; ++level) {
        deepest /= "d";
        ASSERT_TRUE(boost::filesystem::create_directory(deepest));
    }
    ASSERT_TRUE(WriteFile(deepest / "value.json", Bytes("{}")));
    std::vector<uint8_t> encoded;
    ASSERT_TRUE(PackDirectoryToRep(source, &encoded, &error));
    ASSERT_TRUE(WriteFile(input, encoded));
    std::vector<ImportedProfileEntry> results;
    ASSERT_TRUE(ReadImportedProfileResults(input, &results, &error));
    ASSERT_TRUE(UnpackImportedProfileResults(results, output, &error));
    ASSERT_TRUE(SameDirectoryTrees(source, output));

    ASSERT_TRUE(boost::filesystem::create_directory(deepest / "d"));
    ASSERT_TRUE(!PackDirectoryToRep(source, &encoded, &error));
    ASSERT_TRUE(encoded.empty());
    ASSERT_TRUE(error.find("maximum nested report depth exceeded") != std::string::npos);

    ImportedProfileEntry parent;
    parent.name = "d.npu.rep";
    parent.type = NpuRepFileType::NpuRep;
    parent.children = std::move(results);
    results = {std::move(parent)};
    const boost::filesystem::path rejected = temporary.Path() / "rejected";
    ASSERT_TRUE(boost::filesystem::create_directory(rejected));
    ASSERT_TRUE(!UnpackImportedProfileResults(results, rejected, &error));
    ASSERT_TRUE(error.find("maximum nested report depth exceeded") != std::string::npos);
    ASSERT_TRUE(boost::filesystem::is_empty(rejected));
}

TEST(NpuComputeImportedProfileResults, ReadsNestedProfileResults)
{
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    std::vector<uint8_t> encoded;
    ASSERT_TRUE(BuildNestedRep(&encoded));
    const boost::filesystem::path input = temporary.Path() / "input.npu-rep";
    ASSERT_TRUE(WriteFile(input, encoded));

    std::vector<ImportedProfileEntry> results;
    std::string error = "old error";
    ASSERT_TRUE(ReadImportedProfileResults(input, &results, &error));
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(results.size() == 2U);
    ASSERT_TRUE(results[0].name == "HardwareInfo.jsonl");
    ASSERT_TRUE(results[0].type == NpuRepFileType::Jsonl);
    ASSERT_TRUE(results[0].payload == Bytes("{\"category\":\"Host Info\"}\n"));
    ASSERT_TRUE(results[0].children.empty());
    ASSERT_TRUE(results[1].name == "device_0.npu.rep");
    ASSERT_TRUE(results[1].type == NpuRepFileType::NpuRep);
    ASSERT_TRUE(results[1].payload.empty());
    ASSERT_TRUE(results[1].children.size() == 1U);
    ASSERT_TRUE(results[1].children[0].name == "Memory.csv");
}

TEST(NpuComputeImportedProfileResults, UnpacksRecursiveProfileResults)
{
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    const boost::filesystem::path fixture = temporary.Path() / "fixture";
    std::vector<uint8_t> encoded;
    ASSERT_TRUE(BuildRecursiveFixture(fixture, &encoded));
    const boost::filesystem::path input = temporary.Path() / "input.npu-rep";
    ASSERT_TRUE(WriteFile(input, encoded));

    std::vector<ImportedProfileEntry> results;
    std::string error;
    ASSERT_TRUE(ReadImportedProfileResults(input, &results, &error));
    const boost::filesystem::path output = temporary.Path() / "unpacked";
    ASSERT_TRUE(boost::filesystem::create_directory(output));
    ASSERT_TRUE(UnpackImportedProfileResults(results, output, &error));
    ASSERT_TRUE(error.empty());

    std::vector<uint8_t> actual;
    ASSERT_TRUE(ReadFile(output / "HardwareInfo.jsonl", &actual));
    ASSERT_TRUE(actual == HardwareInfoBytes());
    ASSERT_TRUE(ReadFile(output / "metadata.json", &actual));
    ASSERT_TRUE(actual == Bytes("{\"version\":1}\n"));
    ASSERT_TRUE(ReadFile(output / "profile.db", &actual));
    ASSERT_TRUE(actual == std::vector<uint8_t>({0x53U, 0x51U, 0x4cU, 0x00U, 0xffU}));
    ASSERT_TRUE(ReadFile(output / "trace.txt", &actual));
    ASSERT_TRUE(actual == Bytes("instruction text\n"));
    ASSERT_TRUE(ReadFile(output / "device_0" / "Memory.csv", &actual));
    ASSERT_TRUE(actual == Bytes("name,value\nmemory,2\n"));
    ASSERT_TRUE(ReadFile(output / "device_0" / "details" / "L2Cache.csv", &actual));
    ASSERT_TRUE(actual == Bytes("name,value\nl2,3\n"));
    ASSERT_TRUE(RelativeDirectoryEntries(output) == RelativeDirectoryEntries(fixture));
    ASSERT_TRUE(!boost::filesystem::exists(output / ".hardware_info.lock"));
}

TEST(NpuComputeImportedProfileResults, UnpacksShortRepSuffix)
{
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    const boost::filesystem::path output = temporary.Path() / "unpacked";
    ASSERT_TRUE(boost::filesystem::create_directory(output));
    const std::vector<ImportedProfileEntry> results = {
        {"details.rep",
         NpuRepFileType::NpuRep,
         {},
         {{"L2Cache.csv", NpuRepFileType::Csv, Bytes("name,value\nl2,3\n"), {}}}}};
    std::string error;
    ASSERT_TRUE(UnpackImportedProfileResults(results, output, &error));
    std::vector<uint8_t> actual;
    ASSERT_TRUE(ReadFile(output / "details" / "L2Cache.csv", &actual));
    ASSERT_TRUE(actual == Bytes("name,value\nl2,3\n"));
}

TEST(NpuComputeImportedProfileResults, RejectsExistingOutputWithoutPartialWrites)
{
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    const boost::filesystem::path fixture = temporary.Path() / "fixture";
    std::vector<uint8_t> encoded;
    ASSERT_TRUE(BuildRecursiveFixture(fixture, &encoded));
    const boost::filesystem::path input = temporary.Path() / "input.npu-rep";
    ASSERT_TRUE(WriteFile(input, encoded));
    std::vector<ImportedProfileEntry> results;
    std::string error;
    ASSERT_TRUE(ReadImportedProfileResults(input, &results, &error));

    const boost::filesystem::path output = temporary.Path() / "unpacked";
    ASSERT_TRUE(boost::filesystem::create_directory(output));
    const std::vector<uint8_t> existing = Bytes("keep-existing");
    ASSERT_TRUE(WriteFile(output / "trace.txt", existing));
    ASSERT_TRUE(!UnpackImportedProfileResults(results, output, &error));
    ASSERT_TRUE(error.find("exists") != std::string::npos);
    ASSERT_TRUE(!boost::filesystem::exists(output / "HardwareInfo.jsonl"));
    ASSERT_TRUE(!boost::filesystem::exists(output / "device_0"));
    std::vector<uint8_t> actual;
    ASSERT_TRUE(ReadFile(output / "trace.txt", &actual));
    ASSERT_TRUE(actual == existing);
}

TEST(NpuComputeImportedProfileResults, RejectsUnsafeAndConflictingOutputModel)
{
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    const boost::filesystem::path output = temporary.Path() / "unpacked";
    ASSERT_TRUE(boost::filesystem::create_directory(output));
    std::string error;

    std::vector<ImportedProfileEntry> results = {{".hardware_info.lock", NpuRepFileType::Jsonl, Bytes("lock"), {}}};
    ASSERT_TRUE(!UnpackImportedProfileResults(results, output, &error));
    ASSERT_TRUE(error.find("lock") != std::string::npos);
    ASSERT_TRUE(boost::filesystem::is_empty(output));

    results = {{"device_0", NpuRepFileType::Json, Bytes("{}"), {}}, {"device_0.rep", NpuRepFileType::NpuRep, {}, {}}};
    ASSERT_TRUE(!UnpackImportedProfileResults(results, output, &error));
    ASSERT_TRUE(error.find("conflict") != std::string::npos);
    ASSERT_TRUE(boost::filesystem::is_empty(output));

    results = {{"device_0.bin", NpuRepFileType::NpuRep, {}, {}}};
    ASSERT_TRUE(!UnpackImportedProfileResults(results, output, &error));
    ASSERT_TRUE(error.find("invalid nested report entry name") != std::string::npos);
    ASSERT_TRUE(boost::filesystem::is_empty(output));
}

TEST(NpuComputeImportedProfileResults, RejectsInvalidInputAndChildRep)
{
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    std::vector<ImportedProfileEntry> results;
    std::string error;
    ASSERT_TRUE(!ReadImportedProfileResults(temporary.Path() / "missing.npu-rep", &results, &error));
    ASSERT_TRUE(error.find("missing.npu-rep") != std::string::npos);
    ASSERT_TRUE(results.empty());
    ASSERT_TRUE(!ReadImportedProfileResults(temporary.Path(), &results, &error));
    ASSERT_TRUE(error.find("is a directory") != std::string::npos);

    std::vector<uint8_t> child = Bytes("not-a-rep");
    std::vector<uint8_t> parent;
    ASSERT_TRUE(EncodeRep({{"broken.npu.rep", NpuRepFileType::NpuRep, child}}, &parent, &error));
    const boost::filesystem::path input = temporary.Path() / "broken.npu-rep";
    ASSERT_TRUE(WriteFile(input, parent));
    ASSERT_TRUE(!ReadImportedProfileResults(input, &results, &error));
    ASSERT_TRUE(error.find(input.string()) != std::string::npos);
    ASSERT_TRUE(error.find("broken.npu.rep") == std::string::npos);
    ASSERT_TRUE(error.find("is corrupted") != std::string::npos);
    ASSERT_TRUE(error.find("invalid npu-compute report ") != std::string::npos);
    ASSERT_TRUE(results.empty());
}

TEST(NpuComputeImportedProfileResults, InputDiagnosticsAndCompatibleNames)
{
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    const std::string hint = "--import expects a report file, but ";
    std::vector<ImportedProfileEntry> results;
    std::string error;
    const auto missing = temporary.Path() / "app";
    ASSERT_TRUE(!ReadImportedProfileResults(missing, &results, &error));
    ASSERT_TRUE(error == hint + "'" + missing.string() + "' does not exist.");
    ASSERT_TRUE(!ReadImportedProfileResults(temporary.Path(), &results, &error));
    ASSERT_TRUE(error == hint + "'" + temporary.Path().string() + "' is a directory.");
    const auto input = temporary.Path() / "invalid.npu-rep";
    ASSERT_TRUE(WriteFile(input, {}));
    ASSERT_TRUE(!ReadImportedProfileResults(input, &results, &error));
    ASSERT_TRUE(
        error == "invalid npu-compute report '" + input.string() + "': invalid file format: the file is empty.");
    ASSERT_TRUE(WriteFile(input, std::vector<uint8_t>(128, 0)));
    ASSERT_TRUE(!ReadImportedProfileResults(input, &results, &error));
    ASSERT_TRUE(error == "invalid npu-compute report '" + input.string() + "': invalid file format.");
    ASSERT_TRUE(results.empty());
    boost::filesystem::create_symlink(input, temporary.Path() / "link.npu-rep");
    ASSERT_TRUE(!ReadImportedProfileResults(temporary.Path() / "link.npu-rep", &results, &error));
    ASSERT_TRUE(error.find("symbolic links are not supported") != std::string::npos);
    ASSERT_TRUE(error.find(hint) != std::string::npos);

    std::vector<uint8_t> encoded;
    ASSERT_TRUE(BuildNestedRep(&encoded));
    for (const char* name : {"input.npu-rep", "input.npu.rep", "input.rep"}) {
        const auto file = temporary.Path() / name;
        ASSERT_TRUE(WriteFile(file, encoded));
        ASSERT_TRUE(ReadImportedProfileResults(file, &results, &error));
        npucompute::cli::ImportOutputDirectory directory;
        ASSERT_TRUE(
            npucompute::cli::ImportOutputDirectory::Create(file, temporary.Path().string(), &directory, &error));
        ASSERT_TRUE(UnpackImportedProfileResults(results, directory.TemporaryPath(), &error));
        ASSERT_TRUE(boost::filesystem::exists(directory.TemporaryPath() / "device_0" / "Memory.csv"));
    }
    ASSERT_TRUE(WriteFile(input, encoded));
    ASSERT_TRUE(::chmod(temporary.Path().c_str(), 0755) == 0);
    ASSERT_TRUE(::chmod(input.c_str(), 0000) == 0);
    const pid_t child = ::fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0) {
        if (::geteuid() == 0 && (::setgid(65534) != 0 || ::setuid(65534) != 0)) {
            std::perror("drop test privileges");
            ::_exit(2);
        }
        const bool rejected = !ReadImportedProfileResults(input, &results, &error);
        if (!rejected || error.find("cannot be opened") == std::string::npos) {
            std::fprintf(stderr, "unreadable input result: %s\n", error.c_str());
        }
        ::_exit(
            rejected && error.find("cannot be opened") != std::string::npos && error.find(hint) != std::string::npos ?
                0 :
                1);
    }
    int status = 0;
    ASSERT_TRUE(::waitpid(child, &status, 0) == child);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ASSERT_TRUE(::chmod(input.c_str(), 0600) == 0);
    // Linux exposes this as a regular file, but reading unmapped address zero fails.
    ASSERT_TRUE(!ReadImportedProfileResults("/proc/self/mem", &results, &error));
    ASSERT_TRUE(error.find("cannot be read") != std::string::npos);
    ASSERT_TRUE(error.find(hint) != std::string::npos);
    ASSERT_TRUE(results.empty());
}

struct ProcessResult {
    int exit_code = -1;
    std::string standard_error;
};

bool RunCli(
    const boost::filesystem::path& cli, const std::vector<std::string>& arguments,
    const boost::filesystem::path& temporary_root, const boost::filesystem::path& working_directory,
    ProcessResult* result)
{
    int pipe_descriptors[2];
    if (::pipe(pipe_descriptors) != 0) {
        return false;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_descriptors[0]);
        ::close(pipe_descriptors[1]);
        return false;
    }
    if (child == 0) {
        ::close(pipe_descriptors[0]);
        ::dup2(pipe_descriptors[1], STDERR_FILENO);
        ::close(pipe_descriptors[1]);
        ::setenv("TMPDIR", temporary_root.c_str(), 1);
        ::setenv("ACL_API_INJECTION", "/npu-compute-import-must-not-load.so", 1);
        if (::chdir(working_directory.c_str()) != 0) {
            _exit(126);
        }
        std::vector<std::string> storage;
        storage.push_back(cli.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (std::string& argument : storage) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        ::execv(cli.c_str(), argv.data());
        _exit(127);
    }

    ::close(pipe_descriptors[1]);
    char buffer[512];
    while (true) {
        const ssize_t count = ::read(pipe_descriptors[0], buffer, sizeof(buffer));
        if (count > 0) {
            result->standard_error.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(pipe_descriptors[0]);
    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return false;
    }
    result->exit_code = WEXITSTATUS(status);
    return true;
}

bool HasRuntimeCollectionOutput(const std::string& standardError)
{
    return standardError.find("data-directory=") != std::string::npos ||
           standardError.find("report=") != std::string::npos ||
           standardError.find("[prof_api_stub]") != std::string::npos ||
           standardError.find("[aclpti]") != std::string::npos;
}

bool ExtractUnpackedPath(const ProcessResult& result, boost::filesystem::path* output)
{
    constexpr char kPrefix[] = "npu-compute: unpacked= ";
    const std::size_t begin = result.standard_error.find(kPrefix);
    if (begin == std::string::npos || result.standard_error.find(kPrefix, begin + 1U) != std::string::npos) {
        return false;
    }
    const std::size_t pathBegin = begin + sizeof(kPrefix) - 1U;
    const std::size_t end = result.standard_error.find('\n', pathBegin);
    *output = result.standard_error.substr(pathBegin, end - pathBegin);
    return output->is_absolute();
}

bool HasTemporaryImportDirectory(const boost::filesystem::path& outputRoot)
{
    constexpr char kPrefix[] = ".npu-compute-import-";
    for (const boost::filesystem::directory_entry& entry : boost::filesystem::directory_iterator(outputRoot)) {
        const std::string name = entry.path().filename().string();
        if (name.compare(0, sizeof(kPrefix) - 1U, kPrefix) == 0) {
            return true;
        }
    }
    return false;
}

TEST(NpuComputeImportedProfileResults, CliImportUnpacksResults)
{
    const boost::filesystem::path cli(NPU_COMPUTE_CLI_TEST_PATH);
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    const boost::filesystem::path runtime_tmp = temporary.Path() / "tmp";
    const boost::filesystem::path work = temporary.Path() / "work";
    const boost::filesystem::path fixture = temporary.Path() / "fixture";
    ASSERT_TRUE(boost::filesystem::create_directory(runtime_tmp));
    ASSERT_TRUE(boost::filesystem::create_directory(work));
    std::vector<uint8_t> encoded;
    ASSERT_TRUE(BuildRecursiveFixture(fixture, &encoded));
    const boost::filesystem::path input = temporary.Path() / "input.npu-rep";
    ASSERT_TRUE(WriteFile(input, encoded));

    ProcessResult defaultResult;
    ASSERT_TRUE(RunCli(cli, {"--import", input.string()}, runtime_tmp, work, &defaultResult));
    ASSERT_TRUE(defaultResult.exit_code == 0);
    boost::filesystem::path defaultOutput;
    ASSERT_TRUE(ExtractUnpackedPath(defaultResult, &defaultOutput));
    ASSERT_TRUE(defaultOutput.parent_path() == work);
    ASSERT_TRUE(!HasRuntimeCollectionOutput(defaultResult.standard_error));
    ASSERT_TRUE(SameDirectoryTrees(fixture, defaultOutput));
    ASSERT_TRUE(boost::filesystem::is_empty(runtime_tmp));

    const boost::filesystem::path outputRoot = temporary.Path() / "exported";
    ASSERT_TRUE(boost::filesystem::create_directory(outputRoot));
    ASSERT_TRUE(WriteFile(outputRoot / "keep.txt", Bytes("keep")));
    ProcessResult firstResult;
    ASSERT_TRUE(
        RunCli(cli, {"--import", input.string(), "--export", outputRoot.string()}, runtime_tmp, work, &firstResult));
    ASSERT_TRUE(firstResult.exit_code == 0);
    boost::filesystem::path firstOutput;
    ASSERT_TRUE(ExtractUnpackedPath(firstResult, &firstOutput));
    ASSERT_TRUE(firstOutput.parent_path() == outputRoot);
    ASSERT_TRUE(!HasRuntimeCollectionOutput(firstResult.standard_error));
    ASSERT_TRUE(SameDirectoryTrees(fixture, firstOutput));
    ASSERT_TRUE(boost::filesystem::is_empty(runtime_tmp));

    ProcessResult secondResult;
    ASSERT_TRUE(
        RunCli(cli, {"--import", input.string(), "--export", outputRoot.string()}, runtime_tmp, work, &secondResult));
    ASSERT_TRUE(secondResult.exit_code == 0);
    boost::filesystem::path secondOutput;
    ASSERT_TRUE(ExtractUnpackedPath(secondResult, &secondOutput));
    ASSERT_TRUE(secondOutput.parent_path() == outputRoot);
    ASSERT_TRUE(firstOutput != secondOutput);
    ASSERT_TRUE(SameDirectoryTrees(fixture, secondOutput));
    std::vector<uint8_t> actual;
    ASSERT_TRUE(ReadFile(outputRoot / "keep.txt", &actual));
    ASSERT_TRUE(actual == Bytes("keep"));
    ASSERT_TRUE(!HasTemporaryImportDirectory(outputRoot));

    const boost::filesystem::path invalid = temporary.Path() / "invalid.npu-rep";
    ASSERT_TRUE(WriteFile(invalid, Bytes("invalid")));
    ProcessResult invalidResult;
    ASSERT_TRUE(RunCli(cli, {"--import", invalid.string()}, runtime_tmp, work, &invalidResult));
    ASSERT_TRUE(invalidResult.exit_code == 4);
    ASSERT_TRUE(invalidResult.standard_error.find("invalid") != std::string::npos);
    ASSERT_TRUE(!HasTemporaryImportDirectory(work));

    const boost::filesystem::path missingRoot = temporary.Path() / "missing-output-root";
    ProcessResult missingRootResult;
    ASSERT_TRUE(RunCli(
        cli, {"--import", input.string(), "--export", missingRoot.string()}, runtime_tmp, work, &missingRootResult));
    ASSERT_TRUE(missingRootResult.exit_code == 4);
    ASSERT_TRUE(missingRootResult.standard_error.find("does not exist") != std::string::npos);
    ASSERT_TRUE(!boost::filesystem::exists(missingRoot));

    const boost::filesystem::path regularFile = temporary.Path() / "regular-file";
    ASSERT_TRUE(WriteFile(regularFile, Bytes("keep")));
    ProcessResult regularFileResult;
    ASSERT_TRUE(RunCli(
        cli, {"--import", input.string(), "--export", regularFile.string()}, runtime_tmp, work, &regularFileResult));
    ASSERT_TRUE(regularFileResult.exit_code == 4);
    ASSERT_TRUE(regularFileResult.standard_error.find("requires an existing directory") != std::string::npos);
    ASSERT_TRUE(ReadFile(regularFile, &actual));
    ASSERT_TRUE(actual == Bytes("keep"));

    ProcessResult writeFailure;
    ASSERT_TRUE(RunCli(cli, {"--import", input.string(), "--export", "/proc"}, runtime_tmp, work, &writeFailure));
    ASSERT_TRUE(writeFailure.exit_code == 4);
    ASSERT_TRUE(writeFailure.standard_error.find("create import temporary directory failed") != std::string::npos);
    ASSERT_TRUE(boost::filesystem::is_empty(runtime_tmp));
}

} // namespace
