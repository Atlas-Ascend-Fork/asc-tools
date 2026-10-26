/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "pm_sampling/pm_sampling_artifacts.h"
#include <iterator>
#include <memory>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <boost/json.hpp>
#include <boost/filesystem.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <unistd.h>

#define CHECK(x)                                                    \
    do {                                                            \
        if (!(x)) {                                                 \
            std::cerr << "line " << __LINE__ << ": " << #x << '\n'; \
            return 1;                                               \
        }                                                           \
    } while (false)
namespace fs = boost::filesystem;
namespace json = boost::json;
using npucompute::GeneratePmSamplingArtifacts;
using npucompute::PmSamplingArtifactOptions;
using npucompute::PmSamplingArtifacts;
using npucompute::PmSamplingElfSource;
using npucompute::PmSamplingResult;
namespace {
std::string Read(const fs::path& path)
{
    std::ifstream file(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::shared_ptr<const std::vector<char>> Image(const fs::path& path)
{
    const auto bytes = Read(path);
    return std::make_shared<const std::vector<char>>(bytes.begin(), bytes.end());
}
void Write(const fs::path& path, const std::string& bytes)
{
    std::ofstream file(path.string(), std::ios::binary);
    file << bytes;
}
struct Directory {
    fs::path path;
    ~Directory()
    {
        boost::system::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
int Tool(int argc, char** argv)
{
    const std::string mode =
        std::getenv("PM_SAMPLING_TOOL_TEST_MODE") ? std::getenv("PM_SAMPLING_TOOL_TEST_MODE") : "array";
    const bool objdump = fs::path(argv[0]).filename() == "llvm-objdump";
    if (objdump) {
        CHECK(
            argc == 5 && std::string(argv[1]) == "-d" && std::string(argv[2]) == "-l" && std::string(argv[3]) == "-S");
        if (mode == "exit") {
            std::cerr << "objdump failed";
            return 7;
        }
        if (mode == "timeout") {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        if (mode == "large") {
            std::cout << std::string(4096, 'x');
        }
        if (mode != "empty") {
            std::cout << "Disassembly of section .text:\n" << Read(argv[4]);
        }
        return 0;
    }
    CHECK(
        argc == 4 && std::string(argv[1]).find("--obj=") == 0 && std::string(argv[2]) == "--inlining=true" &&
        std::string(argv[3]) == "--output-style=JSON");
    if (mode == "symbol-exit") {
        std::cerr << "symbolizer failed";
        return 8;
    }
    if (mode == "invalid") {
        std::cout << "not JSON";
        return 0;
    }
    if (mode == "truncated") {
        std::cout << "[{\"Address\":";
        return 0;
    }
    if (mode == "missing") {
        std::cout << "[]";
        return 0;
    }
    const auto image = Read(std::string(argv[1]).substr(6));
    json::array records;
    std::string pc;
    while (std::getline(std::cin, pc)) {
        json::array frames;
        for (int i = 0; i < 9; ++i) {
            json::object frame;
            frame["Column"] = 5;
            frame["Discriminator"] = 0;
            frame["FileName"] = mode == "unresolved" ? "" : "/compile machine/source.asc";
            frame["FunctionName"] = "inline_" + std::to_string(i);
            frame["Line"] = mode == "unresolved" ? 0 : 20 + i;
            frame["StartAddress"] = "";
            frame["StartFileName"] = "";
            frame["StartLine"] = 0;
            frames.push_back(std::move(frame));
        }
        json::object record;
        record["Address"] = mode == "wrong" ? "0x123" : pc;
        record["ModuleName"] = image; // Identifies which ELF the generator selected.
        if (mode == "empty-frames") {
            frames.clear();
        }
        record["Symbol"] = std::move(frames);
        record["SelectedImage"] = image;
        records.push_back(std::move(record));
    }
    if (mode == "bad-frames") {
        records[0].as_object()["Symbol"] = json::object{};
    }
    if (mode == "stream") {
        for (const auto& record : records) {
            std::cout << json::serialize(record) << '\n';
        }
    } else {
        std::cout << " \n" << json::serialize(records) << '\n';
    }
    return 0;
}
int Reference(int argc, char** argv)
{
    CHECK(argc == 4);
    fs::create_directories(argv[3]);
    aclptiPmSamplingContext context;
    context.originalElf = context.companionElf = Image(argv[2]);
    context.businessInstructions =
        std::make_shared<const std::map<uint64_t, uint64_t>>(std::map<uint64_t, uint64_t>{{0, 0}, {0x450, 0x450}});
    context.excludedInstructions = std::make_shared<const std::set<uint64_t>>();
    PmSamplingResult sampling;
    sampling.rows[0] = {};
    sampling.rows[0x450] = {};
    PmSamplingArtifacts artifacts;
    PmSamplingArtifactOptions options;
    std::string error;
    CHECK(GeneratePmSamplingArtifacts(argv[3], context, sampling, options, artifacts, error));
    CHECK(fs::path(artifacts.disassemblyPath).filename() == "objdump.txt");
    CHECK(fs::path(artifacts.pcStackPath).filename() == "symbolizer.json");
    const auto records = json::parse(Read(artifacts.pcStackPath)).as_array();
    CHECK(records.size() == 2);
    const auto& frames = records[1].as_object().at("Symbol").as_array();
    CHECK(frames.size() == 9);
    CHECK(frames[0].as_object().at("Line").as_int64() == 35);
    CHECK(frames[8].as_object().at("Line").as_int64() == 336);
    CHECK(frames[4].as_object().at("StartAddress").as_string().empty());
    std::cout << "PASS: reference ELF PC=0x450 nine inline frames preserved, lines=35..336\n";
    return 0;
}
int Real(int argc, char** argv)
{
    CHECK(argc == 5);
    fs::create_directories(argv[4]);
    aclptiPmSamplingContext context;
    context.originalElf = Image(argv[2]);
    context.companionElf = Image(argv[3]);
    context.businessInstructions =
        std::make_shared<const std::map<uint64_t, uint64_t>>(std::map<uint64_t, uint64_t>{{0x10458, 0x268}});
    context.excludedInstructions = std::make_shared<const std::set<uint64_t>>(std::set<uint64_t>{0x900c});
    PmSamplingResult sampling;
    sampling.rows[0x268] = {};
    PmSamplingArtifacts artifacts;
    std::string error;
    PmSamplingArtifactOptions options;
    CHECK(GeneratePmSamplingArtifacts(argv[4], context, sampling, options, artifacts, error));
    const auto parsed = json::parse(Read(artifacts.pcStackPath));
    CHECK(parsed.as_array().size() == 1);
    CHECK(parsed.as_array()[0].as_object().at("Address").as_string() == "0x0000000000000268");
    CHECK(artifacts.unresolvedPcCount == 0);
    std::cout << "real original unresolved=" << artifacts.unresolvedPcCount << '\n';
    options.pcStackSource = PmSamplingElfSource::Companion;
    options.disassemblySource = PmSamplingElfSource::Companion;
    const auto original = fs::path(argv[4]) / "companion";
    fs::create_directory(original);
    CHECK(GeneratePmSamplingArtifacts(original.string(), context, sampling, options, artifacts, error));
    const auto originalParsed = json::parse(Read(artifacts.pcStackPath));
    CHECK(originalParsed.as_array()[0].as_object().at("SymbolizerAddress").as_string() == "0x0000000000010458");
    std::cout << "real companion unresolved=" << artifacts.unresolvedPcCount << '\n';
    std::cout << "PASS: real CANN LLVM artifacts generated in " << argv[4] << '\n';
    return 0;
}
} // namespace
int main(int argc, char** argv)
{
    const auto filename = fs::path(argv[0]).filename();
    if (filename == "llvm-objdump" || filename == "llvm-symbolizer") {
        return Tool(argc, argv);
    }
    if (argc > 1 && std::string(argv[1]) == "--reference") {
        return Reference(argc, argv);
    }
    if (argc > 1 && std::string(argv[1]) == "--real") {
        return Real(argc, argv);
    }
    char pattern[] = "/tmp/pm-sampling-artifacts-XXXXXX";
    const char* created = ::mkdtemp(pattern);
    CHECK(created);
    Directory directory{created};
    const auto tools = directory.path / "tools with spaces;literal";
    fs::create_directory(tools);
    fs::copy_file(fs::canonical("/proc/self/exe"), tools / "llvm-objdump");
    fs::copy_file(fs::canonical("/proc/self/exe"), tools / "llvm-symbolizer");
    aclptiPmSamplingContext context;
    context.originalElf = std::make_shared<const std::vector<char>>(std::vector<char>{'\x7f', 'E', 'L', 'F', 'O'});
    context.companionElf = std::make_shared<const std::vector<char>>(std::vector<char>{'\x7f', 'E', 'L', 'F', 'C'});
    context.businessInstructions = std::make_shared<const std::map<uint64_t, uint64_t>>(
        std::map<uint64_t, uint64_t>{{0x100, 0x10}, {0x108, 0x18}});
    context.excludedInstructions = std::make_shared<const std::set<uint64_t>>(std::set<uint64_t>{0x900c});
    PmSamplingResult sampling;
    sampling.rows[0x10] = {};
    sampling.rows[0x18] = {};
    PmSamplingArtifactOptions options;
    options.toolDirectory = tools.string();
    options.timeoutMs = 1000;
    for (const auto& mode :
         {"array", "stream", "unresolved", "empty-frames", "exit", "symbol-exit", "empty", "timeout", "large",
          "invalid", "truncated", "missing", "wrong", "bad-frames"}) {
        CHECK(::setenv("PM_SAMPLING_TOOL_TEST_MODE", mode, 1) == 0);
        const auto staging = directory.path / mode;
        fs::create_directory(staging);
        PmSamplingArtifacts artifacts;
        std::string error;
        auto limits = options;
        if (std::string(mode) == "timeout") {
            limits.timeoutMs = 20;
        }
        const bool ok = GeneratePmSamplingArtifacts(staging.string(), context, sampling, limits, artifacts, error);
        const bool expected = std::string(mode) == "array" || std::string(mode) == "stream" ||
                              std::string(mode) == "unresolved" || std::string(mode) == "empty-frames" ||
                              std::string(mode) == "large";
        if (ok != expected) {
            std::cerr << "mode=" << mode << " error=" << error << "\n";
        }
        CHECK(ok == expected);
        if (!ok) {
            CHECK(!error.empty());
            const bool disasmFailure =
                std::string(mode) == "exit" || std::string(mode) == "empty" || std::string(mode) == "timeout";
            CHECK(artifacts.disassemblyPath.empty() == disasmFailure);
            CHECK(artifacts.pcStackPath.empty() != disasmFailure);
            CHECK(fs::exists(staging / "objdump.txt") != disasmFailure);
            CHECK(fs::exists(staging / "symbolizer.json") == disasmFailure);
        } else {
            const auto records = json::parse(Read(artifacts.pcStackPath)).as_array();
            CHECK(records.size() == 2);
            CHECK(records[0].as_object().at("Address").as_string() == "0x0000000000000010");
            const auto& frames = records[0].as_object().at("Symbol").as_array();
            if (std::string(mode) == "empty-frames") {
                CHECK(frames.empty());
            } else {
                CHECK(frames.size() == 9 && frames[8].as_object().at("Column").is_int64());
                CHECK(frames[8].as_object().at("StartAddress").as_string().empty());
            }
            CHECK(
                artifacts.unresolvedPcCount ==
                ((std::string(mode) == "unresolved" || std::string(mode) == "empty-frames") ? 2 : 0));
            CHECK(Read(artifacts.disassemblyPath).find("ELFO") != std::string::npos);
        }
        for (const auto& entry : fs::directory_iterator(staging)) {
            CHECK(!fs::is_directory(entry)); // Temporary ELF gone.
        }
    }
    CHECK(::setenv("PM_SAMPLING_TOOL_TEST_MODE", "array", 1) == 0);
    // Independently choose original disassembly and companion symbolization, then vice versa.
    for (int source = 0; source < 2; ++source) {
        auto policy = options;
        policy.disassemblySource = source == 0 ? PmSamplingElfSource::Original : PmSamplingElfSource::Companion;
        policy.pcStackSource = source == 0 ? PmSamplingElfSource::Companion : PmSamplingElfSource::Original;
        const auto staging = directory.path / ("source" + std::to_string(source));
        fs::create_directory(staging);
        PmSamplingArtifacts artifacts;
        std::string error;
        CHECK(GeneratePmSamplingArtifacts(staging.string(), context, sampling, policy, artifacts, error));
        CHECK(Read(artifacts.disassemblyPath).find(source == 0 ? "ELFO" : "ELFC") != std::string::npos);
        const auto records = json::parse(Read(artifacts.pcStackPath)).as_array();
        CHECK(records[0].as_object().at("Address").as_string() == "0x0000000000000010");
        if (source == 0) {
            CHECK(records[0].as_object().at("SymbolizerAddress").as_string() == "0x0000000000000100");
        } else {
            CHECK(!records[0].as_object().contains("SymbolizerAddress"));
        }
        CHECK(records[0].as_object().at("SelectedImage").as_string().back() == (source == 0 ? 'C' : 'O'));
    }
    PmSamplingArtifacts artifacts;
    std::string error;
    sampling.rows[0x900c] = {}; // Never send probe addresses to symbolizer.
    CHECK(!GeneratePmSamplingArtifacts(directory.path.string(), context, sampling, options, artifacts, error));
    sampling.rows.erase(0x900c);
    auto missing = options;
    missing.toolDirectory = (directory.path / "missing").string();
    CHECK(!GeneratePmSamplingArtifacts(directory.path.string(), context, sampling, missing, artifacts, error));
    CHECK(::unsetenv("PM_SAMPLING_TOOL_TEST_MODE") == 0);
    std::cout
        << "PASS: LLVM arguments, source policies, PC mapping, typed JSON, nine frames, limits and failure cleanup\n";
    return 0;
}
