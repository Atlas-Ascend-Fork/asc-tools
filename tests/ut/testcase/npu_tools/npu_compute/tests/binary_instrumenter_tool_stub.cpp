/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
// Controlled compiler/linker executables used by the real binary instrumenter GTest.
#include <boost/filesystem.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string ReadFile(const boost::filesystem::path& path)
{
    std::ifstream input(path.string(), std::ios::binary);
    Require(input.is_open(), "Cannot read " + path.string());
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Require(!input.bad(), "Read failed: " + path.string());
    return bytes;
}

void AppendLine(const boost::filesystem::path& path, const std::string& text)
{
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    Require(descriptor >= 0, "Cannot append " + path.string());
    const std::string line = text + "\n";
    ssize_t written;
    do {
        written = ::write(descriptor, line.data(), line.size());
    } while (written < 0 && errno == EINTR);
    const int closeResult = ::close(descriptor);
    Require(written == static_cast<ssize_t>(line.size()) && closeResult == 0, "Append failed: " + path.string());
}

std::string ArgumentAfter(const std::vector<std::string>& args, const std::string& option, std::size_t offset = 1)
{
    const auto found = std::find(args.begin(), args.end(), option);
    Require(
        found != args.end() && static_cast<std::size_t>(args.end() - found) > offset,
        "Missing argument after " + option);
    return *(found + offset);
}

std::string ArgumentValue(const std::vector<std::string>& args, const std::string& prefix)
{
    const auto found =
        std::find_if(args.begin(), args.end(), [&](const auto& value) { return value.find(prefix) == 0; });
    Require(found != args.end(), "Missing argument " + prefix);
    return found->substr(prefix.size());
}

void AppendLittleEndian(std::string& bytes, std::uint32_t value, unsigned size)
{
    for (unsigned index = 0; index < size; ++index) {
        bytes.push_back(static_cast<char>((value >> (index * 8)) & 0xff));
    }
}

std::string ExpectedControlRecord()
{
    const std::string symbol = "__npu_compute_before_kernel_end";
    std::string bytes;
    AppendLittleEndian(bytes, 12 + 6 + 4 + symbol.size() + 4, 4);
    for (const auto value : {0U, 6U, 0U, 0U, 397U, 0U, 0U}) {
        AppendLittleEndian(bytes, value, 2);
    }
    AppendLittleEndian(bytes, symbol.size() + 4, 4);
    bytes += symbol;
    bytes.append(4, '\0');
    return bytes;
}

std::string ExpectedSimtStartControlRecord()
{
    const std::string symbol = "__npu_compute_after_kernel_stack_frame";
    std::string bytes;
    AppendLittleEndian(bytes, 12 + 8 + 4 + symbol.size() + 4, 4);
    for (const auto value : {0U, 8U, 0U, 0U, 398U, 0U, 1U, 0U}) {
        AppendLittleEndian(bytes, value, 2);
    }
    AppendLittleEndian(bytes, symbol.size() + 4, 4);
    bytes += symbol;
    bytes.append(4, '\0');
    return bytes;
}

void CompileProbe(const boost::filesystem::path& root, const std::vector<std::string>& args)
{
    Require(std::find(args.begin(), args.end(), "-std=c++17") != args.end(), "Probe requires C++17");
    const boost::filesystem::path source = ArgumentAfter(args, "-c");
    Require(source.filename() == "probe.cpp", "Wrong probe source name");
    const std::string probeSource = ReadFile(source);
    const bool simtStart = probeSource.find("__npu_compute_after_kernel_stack_frame") != std::string::npos;
    if (simtStart) {
        Require(probeSource.find("__clang_cce_defines.h") != std::string::npos, "SIMT probe header is missing");
        Require(
            std::find(args.begin(), args.end(), "--cce-aicore-arch=dav-c310-vec") != args.end(),
            "SIMT probe must compile vector-only: a cube stub silently omits async_invoke");
        Require(
            probeSource.find("__simt_vf__ LAUNCH_BOUND(NPU_COMPUTE_SIMT_PROBE_BLOCK_COUNT)[aicore]") !=
                std::string::npos,
            "SIMT probe entry must execute as a SIMT VF");
        Require(
            probeSource.find("unsigned long long pc, unsigned int blockId, unsigned long long pcOffset") !=
                std::string::npos,
            "SIMT probe must declare three implicit ABI parameters before its bound PC offset");
        Require(
            probeSource.find("cce::async_invoke<__npu_compute_simt_start>") != std::string::npos,
            "SIMT probe launch is missing");
        Require(
            probeSource.find("NPU_COMPUTE_SIMT_PROBE_NOP_GROUP_COUNT") != std::string::npos,
            "SIMT probe must contain its delay in the instrumented SIMT entry");
        Require(probeSource.find("pcOffsetAddr") != std::string::npos, "SIMT probe must publish the PC offset");
        Require(probeSource.find("dcci(") != std::string::npos, "SIMT probe must flush its GM metadata");
        Require(
            probeSource.find("__npu_compute_before_kernel_end") == std::string::npos,
            "Sampling companion must not contain the Pipeline end probe");
    } else {
        Require(
            std::find(args.begin(), args.end(), "--npu-arch=dav-3510") != args.end(),
            "Pipeline probe architecture must remain unchanged");
        Require(
            probeSource.find("__npu_compute_before_kernel_end") != std::string::npos,
            "Embedded Pipeline probe source is missing its entry point");
        Require(
            probeSource.find("cce::async_invoke") == std::string::npos,
            "Pipeline probe must not launch the SIMT probe");
    }
    AppendLine(root / "probe_sources", source.parent_path().string());
    std::ofstream output(ArgumentAfter(args, "-o"), std::ios::binary);
    const char elf[8] = {'\x7f', 'E', 'L', 'F', 0, 0, 0, 0};
    output.write(elf, sizeof(elf));
    output.close();
    Require(static_cast<bool>(output), "Cannot write probe object");
}

int LinkProbe(const boost::filesystem::path& root, const std::vector<std::string>& args)
{
    const boost::filesystem::path probe = ArgumentAfter(args, "-execute-probe");
    Require(
        ReadFile(probe).find(std::string(
            "\x7f"
            "ELF",
            4)) == 0,
        "Probe object has no ELF magic");
    Require(boost::filesystem::exists(probe.parent_path() / "probe.cpp"), "Probe source disappeared");
    if (std::find(args.begin(), args.end(), "--symbol-ordering-file") != args.end()) {
        Require(ReadFile(ArgumentAfter(args, "--symbol-ordering-file")) == "kernel\nprobe\n", "Wrong linker ordering");
        Require(std::find(args.begin(), args.end(), "-z") == args.end(), "Unsupported segment option passed to linker");
    }
    AppendLine(root / "caches", probe.parent_path().string());
    const boost::filesystem::path source = ArgumentAfter(args, "-execute-probe", 2);
    AppendLine(root / "workdirs", source.parent_path().string());
    const int marker = ::open((root / "first-attempt").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (marker >= 0) {
        ::close(marker);
        return 1;
    }
    Require(errno == EEXIST, "Cannot create first-link marker");
    boost::filesystem::copy_file(source, ArgumentAfter(args, "-o"));
    return 0;
}

void TuneProbe(const std::vector<std::string>& args)
{
    Require(
        std::find(args.begin(), args.end(), "--tune-argsize=232") != args.end() ||
            std::find(args.begin(), args.end(), "--tune-argsize=48") != args.end(),
        "Wrong trace argument offset");
    const boost::filesystem::path control(ArgumentValue(args, "--dbi-config="));
    const auto bytes = ReadFile(control);
    const bool simtStart = bytes == ExpectedSimtStartControlRecord();
    Require(bytes == ExpectedControlRecord() || simtStart, "Wrong DBI control record");
    if (simtStart) {
        Require(
            std::find(args.begin(), args.end(), "--probe-verb=2") != args.end(),
            "SIMT probe insertion must report the bound hook and insertion count");
    }
    Require(
        (std::find(args.begin(), args.end(), "--kernel-bounded-pcstb-mode") != args.end()) == simtStart,
        "Only SIMT sampling must preserve business instruction scheduling");
    const char* skipSimtInsertion = std::getenv("NPU_COMPUTE_TEST_SIMT_PROBE_NOT_INSERTED");
    if (simtStart) {
        const bool inserted = skipSimtInsertion == nullptr || skipSimtInsertion[0] == '\0';
        std::puts(
            inserted ? "TUNE-INFO: probeFunc: __npu_compute_after_kernel_stack_frame, ManglingName: "
                       "_Z38__npu_compute_after_kernel_stack_framePU3AS1hyjy\n           ApiId: 398  Pc: 0x00000040  "
                       "StubTimes: 1CurFuncName: test_kernel" :
                       "TUNE-INFO: probeFunc: __npu_compute_after_kernel_stack_frame, ManglingName: "
                       "_Z38__npu_compute_after_kernel_stack_framePU3AS1hyjy\n           ApiId: 398  Pc: 0x00000040  "
                       "StubTimes: 0CurFuncName: test_kernel");
    }
    boost::filesystem::copy_file(ArgumentAfter(args, "--instru-memprobe"), ArgumentValue(args, "-o="));
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const char* directory = std::getenv("NPU_COMPUTE_INSTRUMENTER_TEST_ROOT");
        Require(directory != nullptr && directory[0] != '\0', "Missing instrumenter test directory");
        const boost::filesystem::path root(directory);
        const std::string tool = boost::filesystem::path(argv[0]).filename().string();
        const std::vector<std::string> args(argv + 1, argv + argc);
        AppendLine(root / "calls", tool);
        if (tool == "bisheng") {
            CompileProbe(root, args);
        } else if (tool == "ld.lld") {
            return LinkProbe(root, args);
        } else if (tool == "llvm-objdump") {
            const bool probe = boost::filesystem::path(args.back()).filename() == "kernel_end.o";
            std::puts(probe ? "00000000 w F .text 00000008 probe" : "00000000 g F .text 00000008 kernel");
        } else {
            Require(tool == "bisheng-tune", "Unknown tool: " + tool);
            TuneProbe(args);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[binary_instrumenter_tool_stub] %s\n", error.what());
        return 2;
    }
}
