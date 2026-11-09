// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "dbi/dbi_pipeline.h"
#include "../../../common/tests/plog_capture.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace aclsan {
namespace {

void WriteFile(const boost::filesystem::path& path, const std::string& content)
{
    boost::filesystem::create_directories(path.parent_path());
    std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
    output << content;
}

std::string ReadFile(const boost::filesystem::path& path)
{
    std::ifstream input(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void InstallFakeTool(const boost::filesystem::path& path)
{
    WriteFile(path, R"SH(#!/bin/sh
name=$(basename "$0")
printf '%s' "$name" >> "$DBI_FAKE_LOG"
for arg in "$@"; do printf ' <%s>' "$arg" >> "$DBI_FAKE_LOG"; done
printf '\n' >> "$DBI_FAKE_LOG"
if [ "$name" = llvm-objdump ]; then
  printf 'SYMBOL TABLE:\n'
  case "$2" in
    *group.o)
      for symbol in $(sed -n 's/.*void \(__npu_check_probe_[A-Za-z0-9_]*\)(.*/\1/p' "$(dirname "$2")"/src/*.cpp); do
        if [ "$symbol" != "$DBI_FAKE_DROP_SYMBOL" ]; then
          printf '00000000 w F .text.probe 00000010 %s\n' "$symbol"
        fi
      done
      ;;
    *probe.o) printf '00000000 w F .text.probe 00000010 __npu_check_probe_probe\n' ;;
    *)
      if [ -n "$DBI_FAKE_KERNEL_SYMBOLS" ]; then
        printf '%b' "$DBI_FAKE_KERNEL_SYMBOLS"
      else
        printf '00000000 g F .text.kernel 00000010 kernel_main\n'
      fi
      ;;
  esac
  exit 0
fi
output=
previous=
for arg in "$@"; do
  if [ "$previous" = -o ]; then output="$arg"; fi
  case "$arg" in -o=*) output=${arg#-o=} ;; esac
  previous="$arg"
done
if [ -n "$output" ] && [ "$name" != "$DBI_FAKE_SKIP_OUTPUT" ]; then printf 'fake-%s\n' "$name" > "$output"; fi
if [ "$name" = bisheng-tune ]; then
  printf 'first line\n\n'
  printf '%01200d\n' 0
  printf 'last line\n'
  printf 'warning one\nwarning two\n' >&2
fi
if [ "$name" = "$DBI_FAKE_FAIL" ]; then exit 7; fi
)SH");
    ASSERT_EQ(chmod(path.c_str(), 0755), 0);
}

std::size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t position = 0; (position = text.find(needle, position)) != std::string::npos;
         position += needle.size()) {
        ++count;
    }
    return count;
}

TEST(DbiIntegrationTest, CompilesLinksAndPatchesSelectedProbeSet)
{
    PlogCapture capture;
    const auto root = boost::filesystem::temp_directory_path() / "dbi_pipeline_integration";
    boost::filesystem::remove_all(root);
    const auto toolBin = root / "toolchain/tools/bisheng_compiler/bin";
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        InstallFakeTool(toolBin / name);
    }
    WriteFile(root / "input.o", "kernel\n");
    WriteFile(root / "commands.log", "");
    ASSERT_EQ(setenv("DBI_FAKE_LOG", (root / "commands.log").c_str(), 1), 0);

    DbiRequest request{};
    request.inputKernel = (root / "input.o").string();
    request.outputKernel = (root / "patched.o").string();
    request.arch = "dav-3510";
    request.traceArgumentOffset = 24;
    request.probeGroups = {ProbeGroup::Mte2};
    request.toolchainRoot = (root / "toolchain").string();
    request.workDirectory = (root / "work").string();
    request.cacheDirectory = (root / "cache").string();
    request.keepTemp = true;

    testing::internal::CaptureStderr();
    const DbiResult result = RunDbiPipeline(request);
    const std::string console = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(result.success) << result.stage << ": " << result.diagnostic;
    EXPECT_EQ(result.patchedPath, request.outputKernel);
    EXPECT_TRUE(boost::filesystem::is_regular_file(request.outputKernel));
    EXPECT_EQ(console.find("-fno-jump-tables"), std::string::npos) << console;

    const std::string logs = capture.Text();
    EXPECT_NE(logs.find("DBI stage=bisheng-tune output=stdout line=1 part=1 text=first line"), std::string::npos);
    EXPECT_NE(logs.find("DBI stage=bisheng-tune output=stdout line=2 part=1 text=\n"), std::string::npos);
    EXPECT_NE(logs.find("DBI stage=bisheng-tune output=stdout line=3 part=3 text="), std::string::npos);
    EXPECT_NE(logs.find("DBI stage=bisheng-tune output=stdout line=4 part=1 text=last line"), std::string::npos);
    EXPECT_NE(logs.find("DBI stage=bisheng-tune output=stderr line=2 part=1 text=warning two"), std::string::npos);
    std::istringstream records(logs);
    std::string record;
    while (std::getline(records, record)) {
        if (record.empty()) {
            continue;
        }
        EXPECT_LT(record.size(), 1024U);
        EXPECT_NE(record.find("DBI stage="), std::string::npos) << record;
    }

    request.outputKernel = (root / "second-patched.o").string();
    request.workDirectory = (root / "second-work").string();
    const DbiResult cachedResult = RunDbiPipeline(request);
    EXPECT_TRUE(cachedResult.success) << cachedResult.stage << ": " << cachedResult.diagnostic;
    EXPECT_EQ(cachedResult.patchedPath, request.outputKernel);
    EXPECT_TRUE(boost::filesystem::is_regular_file(request.outputKernel));

    const std::string commands = ReadFile(root / "commands.log");
    EXPECT_NE(commands.find("bisheng <-xcce>"), std::string::npos) << commands;
    EXPECT_NE(
        commands.find("<" + (root / "toolchain/x86_64-linux/asc/impl/basic_api").string() + ">"), std::string::npos)
        << commands;
    EXPECT_NE(commands.find("ld.lld <-r>"), std::string::npos) << commands;
    EXPECT_NE(commands.find("ld.lld <-r> <-m> <aicorelinux>"), std::string::npos) << commands;
    EXPECT_NE(commands.find("generated-mte2.cpp"), std::string::npos) << commands;
    EXPECT_NE(commands.find("generated-scalar.cpp"), std::string::npos) << commands;
    EXPECT_NE(commands.find("llvm-objdump <--syms>"), std::string::npos) << commands;
    EXPECT_NE(commands.find("<-execute-probe>"), std::string::npos) << commands;
    EXPECT_NE(commands.find("bisheng-tune <--action=instru-probe>"), std::string::npos) << commands;
    EXPECT_NE(commands.find("<--tune-argsize=24>"), std::string::npos) << commands;
    EXPECT_NE(commands.find("<--dbi-config="), std::string::npos) << commands;
    EXPECT_EQ(CountOccurrences(commands, "bisheng <-xcce>"), 2U) << commands;
    EXPECT_EQ(CountOccurrences(commands, "ld.lld <-r>"), 1U) << commands;
    EXPECT_EQ(CountOccurrences(commands, "bisheng-tune <--action=instru-probe>"), 2U) << commands;
    unsetenv("DBI_FAKE_LOG");
    boost::filesystem::remove_all(root);
}

class JumpTableSymbolTest : public testing::Test {
protected:
    void SetUp() override
    {
        root_ = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dbi-jump-table-%%%%-%%%%");
        const auto toolBin = root_ / "toolchain/tools/bisheng_compiler/bin";
        for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
            InstallFakeTool(toolBin / name);
        }
        WriteFile(root_ / "input.o", "kernel\n");
        WriteFile(root_ / "commands.log", "");
        ASSERT_EQ(setenv("DBI_FAKE_LOG", (root_ / "commands.log").c_str(), 1), 0);
        request_.inputKernel = (root_ / "input.o").string();
        request_.outputKernel = (root_ / "patched.o").string();
        request_.arch = "dav-3510";
        request_.traceArgumentOffset = 24;
        request_.probeGroups = {ProbeGroup::Mte2};
        request_.toolchainRoot = (root_ / "toolchain").string();
        request_.workDirectory = (root_ / "work").string();
        request_.cacheDirectory = (root_ / "cache").string();
    }

    void TearDown() override
    {
        unsetenv("DBI_FAKE_KERNEL_SYMBOLS");
        unsetenv("DBI_FAKE_LOG");
        boost::filesystem::remove_all(root_);
    }

    void SetSymbols(const std::string& row)
    {
        const std::string symbols = "00000000 g F .text.kernel 00000010 kernel_main\n" + row;
        ASSERT_EQ(setenv("DBI_FAKE_KERNEL_SYMBOLS", symbols.c_str(), 1), 0);
    }

    boost::filesystem::path root_;
    DbiRequest request_;
};

class JumpTableRejectionTest : public JumpTableSymbolTest, public testing::WithParamInterface<const char*> {};

int32_t EnableLogLevel(int32_t, int32_t) { return 1; }

void RecordJumpTableError(int32_t, int32_t level, const char* format, ...)
{
    if (level != DLOG_ERROR) {
        return;
    }
    char message[2048] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    const char* log = std::getenv("DBI_FAKE_LOG");
    if (log != nullptr) {
        std::ofstream output(std::string(log) + ".plog", std::ios::app);
        output << message;
    }
}

TEST_P(JumpTableRejectionTest, ExitsBeforeCompilingOrRunningKernel)
{
    SetSymbols(GetParam());
    // The child must exit inside the pipeline even if its caller would ignore a returned failure.
    EXPECT_EXIT(
        {
            plog_test::SetApi({EnableLogLevel, RecordJumpTableError});
            RunDbiPipeline(request_);
            _exit(0);
        },
        testing::ExitedWithCode(EXIT_FAILURE), "npu_check: ERROR:.*JTI.*-fno-jump-tables");

    const std::string commands = ReadFile(root_ / "commands.log");
    EXPECT_EQ(CountOccurrences(commands, "llvm-objdump <--syms>"), 1U) << commands;
    EXPECT_EQ(commands.find("bisheng <"), std::string::npos) << commands;
    EXPECT_EQ(commands.find("ld.lld <"), std::string::npos) << commands;
    EXPECT_EQ(commands.find("bisheng-tune <"), std::string::npos) << commands;
    EXPECT_FALSE(boost::filesystem::exists(request_.outputKernel));
    EXPECT_FALSE(boost::filesystem::exists(request_.cacheDirectory));
    const std::string errors = ReadFile(root_ / "commands.log.plog");
    EXPECT_NE(errors.find("kernel contains a jump table"), std::string::npos) << errors;
    EXPECT_NE(errors.find("-fno-jump-tables"), std::string::npos) << errors;
}

INSTANTIATE_TEST_SUITE_P(
    DefinedJumpTables, JumpTableRejectionTest,
    testing::Values(
        "00000000000001e0 l       .rodata\t0000000000000000 JTI0_0\n", "00000040 l O .rodata 00000010 JTI12_34\n",
        "00000040 l O .rodata 00000010 JTI\n", "00000040 l O .rodata 00000010 JTI42\n",
        "00000040 l O .rodata 00000010 JTI0_\n", "00000040 l O .rodata 00000010 JTI0_0_extra\n",
        "00000040 l   .rodata 00000010 JTIx_0\n"));

class NonJumpTableSymbolTest : public JumpTableSymbolTest, public testing::WithParamInterface<const char*> {};

TEST_P(NonJumpTableSymbolTest, DoesNotRejectUnrelatedNamesOrNonSymbolText)
{
    SetSymbols(GetParam());
    PlogCapture capture;
    testing::internal::CaptureStderr();
    const DbiResult result = RunDbiPipeline(request_);
    const std::string console = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(result.success) << result.stage << ": " << result.diagnostic;
    EXPECT_EQ(console.find("-fno-jump-tables"), std::string::npos) << console;
    EXPECT_EQ(capture.Text().find("kernel contains a jump table"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(
    SymbolTableBoundaries, NonJumpTableSymbolTest,
    testing::Values(
        "00000040 l O .rodata 00000010 jti0_0\n", "00000040 l O .rodata 00000010 user_JTI0_0\n",
        "00000000         *UND* 00000000 JTI0_0\n", "00000000 l    df *ABS* 00000000 JTI0_0\n",
        "00000000 l    d  .rodata 00000000 JTI0_0\n", "00000000 l    F  .text 00000010 JTI0_0\n",
        "00000040 l O JTI0_0 00000010 regular_name\n", "00000040 l O .rodata 00000010 regular_name JTI0_0\n",
        "/tmp/JTI0_0\n", "note: JTI0_0\n"));

TEST(DbiIntegrationTest, KeepsFirstKernelAsOrderingAnchorAndPassesSelectedTilingKeyToTune)
{
    const auto root = boost::filesystem::temp_directory_path() / "dbi_pipeline_tiling_symbol";
    boost::filesystem::remove_all(root);
    const auto toolBin = root / "toolchain/tools/bisheng_compiler/bin";
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        InstallFakeTool(toolBin / name);
    }
    WriteFile(root / "input.o", "kernel\n");
    WriteFile(root / "commands.log", "");
    ASSERT_EQ(setenv("DBI_FAKE_LOG", (root / "commands.log").c_str(), 1), 0);
    ASSERT_EQ(
        setenv(
            "DBI_FAKE_KERNEL_SYMBOLS",
            "00000000 g F .text.kernel 00000010 Multi_1_mix_aic\\n"
            "00000010 g F .text.kernel 00000010 Multi_1_mix_aiv\\n"
            "00000020 g F .text.kernel 00000010 Multi_3_mix_aiv\\n"
            "00000030 g F .text.kernel 00000010 Multi_3_mix_aic\\n",
            1),
        0);

    DbiRequest request{};
    request.inputKernel = (root / "input.o").string();
    request.outputKernel = (root / "patched.o").string();
    request.arch = "dav-3510";
    request.traceArgumentOffset = 40;
    request.probeGroups = {ProbeGroup::Mte2};
    request.toolchainRoot = (root / "toolchain").string();
    request.workDirectory = (root / "work").string();
    request.cacheDirectory = (root / "cache").string();
    request.keepTemp = true;
    request.tilingKey = 3;

    const DbiResult result = RunDbiPipeline(request);
    ASSERT_TRUE(result.success) << result.stage << ": " << result.diagnostic;
    const std::string ordering = ReadFile(root / "work/symbol_ordering.txt");
    EXPECT_EQ(ordering.substr(0, ordering.find('\n')), "Multi_1_mix_aic");
    EXPECT_NE(ReadFile(root / "commands.log").find("<--tiling-key=3>"), std::string::npos);

    unsetenv("DBI_FAKE_KERNEL_SYMBOLS");
    unsetenv("DBI_FAKE_LOG");
    boost::filesystem::remove_all(root);
}

TEST(DbiIntegrationTest, FailedProbeLinkDoesNotPublishPartialCacheArtifact)
{
    const auto root = boost::filesystem::temp_directory_path() / "dbi_pipeline_failed_cache";
    boost::filesystem::remove_all(root);
    const auto toolBin = root / "toolchain/tools/bisheng_compiler/bin";
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        InstallFakeTool(toolBin / name);
    }
    WriteFile(root / "input.o", "kernel\n");
    WriteFile(root / "commands.log", "");
    ASSERT_EQ(setenv("DBI_FAKE_LOG", (root / "commands.log").c_str(), 1), 0);
    ASSERT_EQ(setenv("DBI_FAKE_FAIL", "ld.lld", 1), 0);

    DbiRequest request{};
    request.inputKernel = (root / "input.o").string();
    request.outputKernel = (root / "first-patched.o").string();
    request.arch = "dav-3510";
    request.probeGroups = {ProbeGroup::Mte2};
    request.toolchainRoot = (root / "toolchain").string();
    request.workDirectory = (root / "first-work").string();
    request.cacheDirectory = (root / "cache").string();

    const DbiResult failed = RunDbiPipeline(request);
    EXPECT_FALSE(failed.success);
    EXPECT_EQ(failed.stage, "link-probe");

    ASSERT_EQ(unsetenv("DBI_FAKE_FAIL"), 0);
    request.outputKernel = (root / "second-patched.o").string();
    request.workDirectory = (root / "second-work").string();
    const DbiResult retried = RunDbiPipeline(request);
    EXPECT_TRUE(retried.success) << retried.stage << ": " << retried.diagnostic;

    const std::string commands = ReadFile(root / "commands.log");
    EXPECT_EQ(CountOccurrences(commands, "ld.lld <-r>"), 2U) << commands;
    unsetenv("DBI_FAKE_LOG");
    boost::filesystem::remove_all(root);
}

TEST(DbiIntegrationTest, DoesNotAcceptStalePatchedOutputWhenTuneCreatesNothing)
{
    const auto root = boost::filesystem::temp_directory_path() / "dbi_pipeline_stale_output";
    boost::filesystem::remove_all(root);
    const auto toolBin = root / "toolchain/tools/bisheng_compiler/bin";
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        InstallFakeTool(toolBin / name);
    }
    WriteFile(root / "input.o", "kernel\n");
    WriteFile(root / "patched.o", "stale\n");
    WriteFile(root / "commands.log", "");
    ASSERT_EQ(setenv("DBI_FAKE_LOG", (root / "commands.log").c_str(), 1), 0);
    ASSERT_EQ(setenv("DBI_FAKE_SKIP_OUTPUT", "bisheng-tune", 1), 0);

    DbiRequest request{};
    request.inputKernel = (root / "input.o").string();
    request.outputKernel = (root / "patched.o").string();
    request.arch = "dav-3510";
    request.probeGroups = {ProbeGroup::Mte2};
    request.toolchainRoot = (root / "toolchain").string();
    request.workDirectory = (root / "work").string();
    request.cacheDirectory = (root / "cache").string();

    const DbiResult result = RunDbiPipeline(request);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.stage, "bisheng-tune");
    EXPECT_EQ(ReadFile(request.outputKernel), "stale\n");

    unsetenv("DBI_FAKE_SKIP_OUTPUT");
    unsetenv("DBI_FAKE_LOG");
    boost::filesystem::remove_all(root);
}

TEST(DbiIntegrationTest, RebuildsNonemptyCorruptAggregateCacheAsOneUnit)
{
    const auto root = boost::filesystem::temp_directory_path() / "dbi_pipeline_corrupt_aggregate";
    boost::filesystem::remove_all(root);
    const auto toolBin = root / "toolchain/tools/bisheng_compiler/bin";
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        InstallFakeTool(toolBin / name);
    }
    WriteFile(root / "input.o", "kernel\n");
    WriteFile(root / "commands.log", "");
    ASSERT_EQ(setenv("DBI_FAKE_LOG", (root / "commands.log").c_str(), 1), 0);

    DbiRequest request{};
    request.inputKernel = (root / "input.o").string();
    request.outputKernel = (root / "first-patched.o").string();
    request.arch = "dav-3510";
    request.probeGroups = {ProbeGroup::Mte2};
    request.toolchainRoot = (root / "toolchain").string();
    request.workDirectory = (root / "first-work").string();
    request.cacheDirectory = (root / "cache").string();

    const DbiResult first = RunDbiPipeline(request);
    ASSERT_TRUE(first.success) << first.stage << ": " << first.diagnostic;
    struct stat cacheStatus {};
    ASSERT_EQ(stat((root / "cache").c_str(), &cacheStatus), 0);
    EXPECT_EQ(cacheStatus.st_mode & 0777, 0700);
    boost::filesystem::path artifactDirectory;
    for (const auto& entry : boost::filesystem::directory_iterator(root / "cache/aggregates")) {
        if (entry.is_directory()) {
            artifactDirectory = entry.path();
            break;
        }
    }
    ASSERT_FALSE(artifactDirectory.empty());
    struct stat artifactStatus {};
    ASSERT_EQ(stat(artifactDirectory.c_str(), &artifactStatus), 0);
    EXPECT_EQ(artifactStatus.st_mode & 0777, 0700);
    ASSERT_TRUE(boost::filesystem::is_regular_file(artifactDirectory / "manifest"));
    WriteFile(artifactDirectory / "ctrl.bin", "nonempty-corruption\n");

    request.outputKernel = (root / "second-patched.o").string();
    request.workDirectory = (root / "second-work").string();
    const DbiResult second = RunDbiPipeline(request);
    EXPECT_TRUE(second.success) << second.stage << ": " << second.diagnostic;
    EXPECT_EQ(CountOccurrences(ReadFile(root / "commands.log"), "ld.lld <-r>"), 2U);
    EXPECT_EQ(CountOccurrences(ReadFile(root / "commands.log"), "bisheng <-xcce>"), 2U);

    unsetenv("DBI_FAKE_LOG");
    boost::filesystem::remove_all(root);
}

TEST(DbiIntegrationTest, RejectsSymlinkCacheRoot)
{
    const auto root = boost::filesystem::temp_directory_path() / "dbi_pipeline_symlink_cache";
    boost::filesystem::remove_all(root);
    const auto toolBin = root / "toolchain/tools/bisheng_compiler/bin";
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        InstallFakeTool(toolBin / name);
    }
    WriteFile(root / "input.o", "kernel\n");
    boost::filesystem::create_directories(root / "redirected-cache");
    boost::filesystem::create_directory_symlink(root / "redirected-cache", root / "cache-link");

    DbiRequest request{};
    request.inputKernel = (root / "input.o").string();
    request.outputKernel = (root / "patched.o").string();
    request.arch = "dav-3510";
    request.probeGroups = {ProbeGroup::Mte2};
    request.toolchainRoot = (root / "toolchain").string();
    request.workDirectory = (root / "work").string();
    request.cacheDirectory = (root / "cache-link").string();

    const DbiResult result = RunDbiPipeline(request);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.stage, "cache-directory");
    EXPECT_TRUE(boost::filesystem::is_empty(root / "redirected-cache"));
    boost::filesystem::remove_all(root);
}

TEST(DbiIntegrationTest, RejectsUnsupportedGeneratedProbeArchitecture)
{
    const auto root = boost::filesystem::temp_directory_path() / "dbi_pipeline_unsupported_arch";
    boost::filesystem::remove_all(root);
    const auto toolBin = root / "toolchain/tools/bisheng_compiler/bin";
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        InstallFakeTool(toolBin / name);
    }
    WriteFile(root / "input.o", "kernel\n");

    DbiRequest request{};
    request.inputKernel = (root / "input.o").string();
    request.outputKernel = (root / "patched.o").string();
    request.arch = "dav-unknown";
    request.probeGroups = {ProbeGroup::Sync};
    request.toolchainRoot = (root / "toolchain").string();
    request.workDirectory = (root / "work").string();
    request.cacheDirectory = (root / "cache").string();

    const DbiResult result = RunDbiPipeline(request);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.stage, "architecture");
    EXPECT_NE(result.diagnostic.find("dav-unknown"), std::string::npos);
    boost::filesystem::remove_all(root);
}

TEST(DbiIntegrationTest, RejectsGeneratedGroupWithMissingWeakSymbol)
{
    const auto root = boost::filesystem::temp_directory_path() / "dbi_pipeline_missing_generated_symbol";
    boost::filesystem::remove_all(root);
    const auto toolBin = root / "toolchain/tools/bisheng_compiler/bin";
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        InstallFakeTool(toolBin / name);
    }
    WriteFile(root / "input.o", "kernel\n");
    WriteFile(root / "commands.log", "");
    ASSERT_EQ(setenv("DBI_FAKE_LOG", (root / "commands.log").c_str(), 1), 0);
    ASSERT_EQ(setenv("DBI_FAKE_DROP_SYMBOL", "__npu_check_probe_copy_ubuf_to_cbuf", 1), 0);

    DbiRequest request{};
    request.inputKernel = (root / "input.o").string();
    request.outputKernel = (root / "patched.o").string();
    request.arch = "dav-3510";
    request.probeGroups = {ProbeGroup::Mte3};
    request.toolchainRoot = (root / "toolchain").string();
    request.workDirectory = (root / "work").string();
    request.cacheDirectory = (root / "cache").string();
    request.keepTemp = true;

    const DbiResult result = RunDbiPipeline(request);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.stage, "validate-probe");
    bool retainedStaging = false;
    for (const auto& entry : boost::filesystem::directory_iterator(root / "cache/groups")) {
        retainedStaging = retainedStaging || entry.path().filename().string().compare(0, 7, ".build-") == 0;
    }
    EXPECT_TRUE(retainedStaging);

    unsetenv("DBI_FAKE_DROP_SYMBOL");
    unsetenv("DBI_FAKE_LOG");
    boost::filesystem::remove_all(root);
}

} // namespace
} // namespace aclsan
