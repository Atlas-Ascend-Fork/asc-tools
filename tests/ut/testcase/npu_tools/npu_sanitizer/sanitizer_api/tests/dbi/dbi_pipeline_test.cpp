// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "dbi/dbi_pipeline.h"
#include "dbi/ctrlbin_generator.h"
#include "dbi/probe_source_generator.h"
#include "dbi/tool_runner.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace aclsan {
namespace {

TEST(DbiPipelineTest, NormalizesProbeGroupsAndRejectsDuplicates)
{
    const auto groups = NormalizeProbeGroups({ProbeGroup::Sync, ProbeGroup::Mte2, ProbeGroup::Sync});
    ASSERT_EQ(groups.size(), 3U);
    EXPECT_EQ(groups[0], ProbeGroup::Mte2);
    EXPECT_EQ(groups[1], ProbeGroup::Scalar);
    EXPECT_EQ(groups[2], ProbeGroup::Sync);
}

TEST(DbiPipelineTest, AddsScalarDependencyForEveryMemoryConsumerGroup)
{
    for (const ProbeGroup consumer : {ProbeGroup::Mte2, ProbeGroup::Mte3, ProbeGroup::Fixpipe}) {
        const auto groups = NormalizeProbeGroups({consumer});
        ASSERT_EQ(groups.size(), 2U) << ProbeGroupName(consumer);
        EXPECT_NE(std::find(groups.begin(), groups.end(), consumer), groups.end()) << ProbeGroupName(consumer);
        EXPECT_NE(std::find(groups.begin(), groups.end(), ProbeGroup::Scalar), groups.end())
            << ProbeGroupName(consumer);
    }
}

TEST(DbiPipelineTest, ObjectAndCtrlbinPlansAgreeForEveryProbeMask)
{
    const std::pair<uint32_t, ProbeGroup> groupBits[] = {
        {PROBE_GROUP_MTE1, ProbeGroup::Mte1},     {PROBE_GROUP_MTE2, ProbeGroup::Mte2},
        {PROBE_GROUP_MTE3, ProbeGroup::Mte3},     {PROBE_GROUP_FIXPIPE, ProbeGroup::Fixpipe},
        {PROBE_GROUP_SYNC, ProbeGroup::Sync},     {PROBE_GROUP_SCALAR, ProbeGroup::Scalar},
        {PROBE_GROUP_MATRIX, ProbeGroup::Matrix}, {PROBE_GROUP_VECTOR, ProbeGroup::Vector},
    };
    for (uint32_t mask = 0; mask <= PROBE_GROUP_ALL; ++mask) {
        std::vector<ProbeGroup> rawGroups;
        for (const auto& [bit, group] : groupBits) {
            if ((mask & bit) != 0U) {
                rawGroups.push_back(group);
            }
        }
        const auto normalized = NormalizeProbeGroups(rawGroups);
        EXPECT_EQ(ProbeGroupsFromMask(mask), normalized) << "mask=" << mask;
        EXPECT_EQ(BindingSymbols(rawGroups), BindingSymbols(normalized)) << "mask=" << mask;
    }
}

TEST(DbiPipelineTest, NamesNewPipelineGroups)
{
    EXPECT_EQ(ProbeGroupName(ProbeGroup::Matrix), "matrix");
    EXPECT_EQ(ProbeGroupName(ProbeGroup::Vector), "vector");
    EXPECT_EQ(PROBE_GROUP_ALL & PROBE_GROUP_MATRIX, PROBE_GROUP_MATRIX);
    EXPECT_EQ(PROBE_GROUP_ALL & PROBE_GROUP_VECTOR, PROBE_GROUP_VECTOR);
}

TEST(DbiPipelineTest, ValidatesRequestFields)
{
    DbiRequest request{};
    request.inputKernel = "/tmp/input.o";
    request.outputKernel = "/tmp/output.o";
    request.arch = "dav-3510";
    request.probeGroups = {ProbeGroup::Mte2};
    EXPECT_TRUE(ValidateRequest(request).empty());

    request.arch.clear();
    EXPECT_EQ(ValidateRequest(request), "architecture is empty");
}

TEST(DbiPipelineTest, ResolvesToolchainFromExplicitRoot)
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() / "dbi_toolchain_test";
    boost::filesystem::remove_all(root);
    boost::filesystem::create_directories(root / "tools/bisheng_compiler/bin");
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        std::ofstream((root / "tools/bisheng_compiler/bin" / name).string()).put('\n');
    }

    const auto toolchain = ResolveToolchain(root.string());
    EXPECT_EQ(toolchain.bisheng, (root / "tools/bisheng_compiler/bin/bisheng").string());
    EXPECT_EQ(toolchain.bishengTune, (root / "tools/bisheng_compiler/bin/bisheng-tune").string());
    EXPECT_EQ(toolchain.ldLld, (root / "tools/bisheng_compiler/bin/ld.lld").string());
    EXPECT_EQ(toolchain.llvmObjdump, (root / "tools/bisheng_compiler/bin/llvm-objdump").string());
    EXPECT_FALSE(toolchain.Complete());
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        ASSERT_EQ(chmod((root / "tools/bisheng_compiler/bin" / name).c_str(), 0755), 0);
    }
    EXPECT_TRUE(toolchain.Complete());
    boost::filesystem::remove_all(root);
}

TEST(DbiPipelineTest, ResolvesToolchainWithSymlinkedExecutable)
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() / "dbi_toolchain_symlink_test";
    boost::filesystem::remove_all(root);
    const auto bin = root / "tools/bisheng_compiler/bin";
    boost::filesystem::create_directories(bin);
    for (const char* name : {"bisheng", "bisheng-tune", "lld", "llvm-objdump"}) {
        std::ofstream((bin / name).string()).put('\n');
        ASSERT_EQ(chmod((bin / name).c_str(), 0755), 0);
    }
    boost::filesystem::create_symlink("lld", bin / "ld.lld");

    const auto toolchain = ResolveToolchain(root.string());
    EXPECT_EQ(toolchain.ldLld, (bin / "ld.lld").string());
    EXPECT_TRUE(toolchain.Complete());
    boost::filesystem::remove_all(root);
}

TEST(DbiPipelineTest, DoesNotMixToolsFromDifferentRoots)
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() / "dbi_toolchain_mixed_test";
    boost::filesystem::remove_all(root);
    const auto first = root / "first/tools/bisheng_compiler/bin";
    const auto second = root / "second";
    boost::filesystem::create_directories(first);
    boost::filesystem::create_directories(second);
    for (const char* name : {"bisheng", "bisheng-tune"}) {
        std::ofstream((first / name).string()).put('\n');
        ASSERT_EQ(chmod((first / name).c_str(), 0755), 0);
    }
    for (const char* name : {"ld.lld", "llvm-objdump"}) {
        std::ofstream((second / name).string()).put('\n');
        ASSERT_EQ(chmod((second / name).c_str(), 0755), 0);
    }

    const auto toolchain = ResolveToolchain((root / "first").string());
    EXPECT_FALSE(toolchain.Complete());
    boost::filesystem::remove_all(root);
}

TEST(DbiPipelineTest, RejectsWorldWritableToolchainExecutable)
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() / "dbi_toolchain_permissions_test";
    boost::filesystem::remove_all(root);
    const auto bin = root / "tools/bisheng_compiler/bin";
    boost::filesystem::create_directories(bin);
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        std::ofstream((bin / name).string()).put('\n');
        ASSERT_EQ(chmod((bin / name).c_str(), 0755), 0);
    }
    ASSERT_EQ(chmod((bin / "bisheng").c_str(), 0777), 0);

    EXPECT_FALSE(ResolveToolchain(root.string()).Complete());
    boost::filesystem::remove_all(root);
}

TEST(DbiPipelineTest, ResolvesCannRootFromLoadedRuntimeLibrary)
{
    const auto root = boost::filesystem::temp_directory_path() / "dbi-runtime-root-test";
    boost::filesystem::remove_all(root);
    const auto tools = root / "tools/bisheng_compiler/bin";
    const auto runtime = root / "x86_64-linux/lib64/libacl_rt.so";
    boost::filesystem::create_directories(tools);
    boost::filesystem::create_directories(runtime.parent_path());
    std::ofstream(runtime.string()).put('\n');
    for (const char* name : {"bisheng", "bisheng-tune", "ld.lld", "llvm-objdump"}) {
        std::ofstream((tools / name).string()).put('\n');
    }
    EXPECT_EQ(CannRootFromRuntimeLibrary(runtime.string()), root.string());
    EXPECT_TRUE(CannRootFromRuntimeLibrary("/usr/lib/libacl_rt.so").empty());
    boost::filesystem::remove_all(root);
}

TEST(DbiPipelineTest, CacheKeyChangesWithProbeSetAndObjectIdentity)
{
    const auto first = MakeCacheKey("dav-3510", {ProbeGroup::Mte2}, "objects-a");
    const auto second = MakeCacheKey("dav-3510", {ProbeGroup::Mte3}, "objects-a");
    const auto third = MakeCacheKey("dav-3510", {ProbeGroup::Mte2}, "objects-b");
    const auto aic = MakeCacheKey("dav-3510", {ProbeGroup::Mte2}, "objects-a", ProbeCoreType::Aic);
    const auto aiv = MakeCacheKey("dav-3510", {ProbeGroup::Mte2}, "objects-a", ProbeCoreType::Aiv);
    EXPECT_NE(first, second);
    EXPECT_NE(first, third);
    EXPECT_NE(aic, aiv);
}

TEST(DbiPipelineTest, DetectsProbeCoreTypeFromSelectedKernelAndPipeSymbols)
{
    EXPECT_EQ(DetectProbeCoreType("g_cubeTPipePtr\n", "Dn2nzCustom_0"), ProbeCoreType::Aic);
    EXPECT_EQ(DetectProbeCoreType("g_vecTPipePtr\n", "VectorCustom_0"), ProbeCoreType::Aiv);
    EXPECT_EQ(DetectProbeCoreType("g_cubeTPipePtr\ng_vecTPipePtr\n", "MixCustom_3_mix_aic"), ProbeCoreType::Mix);
    EXPECT_EQ(DetectProbeCoreType("", "Standalone_aic"), ProbeCoreType::Aic);
    EXPECT_EQ(DetectProbeCoreType("", "Standalone_aiv"), ProbeCoreType::Aiv);
    EXPECT_EQ(DetectProbeCoreType("", "UnknownCustom_0"), ProbeCoreType::Mix);
}

TEST(DbiPipelineTest, CompleteProbeSetCoversAicAndMixKernels)
{
    const std::vector<ProbeGroup> requested{ProbeGroup::Mte2};
    EXPECT_EQ(ResolveProbeGroups(requested, ProbeCoreType::Aic, true), AllProbeGroups());
    EXPECT_EQ(ResolveProbeGroups(requested, ProbeCoreType::Mix, true), AllProbeGroups());
    EXPECT_EQ(
        ResolveProbeGroups(requested, ProbeCoreType::Aiv, true),
        (std::vector<ProbeGroup>{ProbeGroup::Mte2, ProbeGroup::Scalar}));
    EXPECT_EQ(
        ResolveProbeGroups(requested, ProbeCoreType::Aic, false),
        (std::vector<ProbeGroup>{ProbeGroup::Mte2, ProbeGroup::Scalar}));
}

std::string ReadGeneratedFile(const boost::filesystem::path& path)
{
    std::ifstream input(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(CtrlbinGeneratorTest, FiltersBindingsToSelectedProbeGroups)
{
    const auto symbols = BindingSymbols({ProbeGroup::Mte2});
    ASSERT_FALSE(symbols.empty());
    for (const auto& symbol : symbols) {
        EXPECT_NE(symbol.find("sanitizer_report_"), std::string::npos);
    }

    const auto path = boost::filesystem::temp_directory_path() / "mte2-only.ctrl.bin";
    boost::filesystem::remove(path);
    std::string diagnostic;
    EXPECT_TRUE(GenerateCtrlBin(path.string(), {ProbeGroup::Mte2}, diagnostic)) << diagnostic;
    EXPECT_GT(boost::filesystem::file_size(path), 0U);
    boost::filesystem::remove(path);

    const auto mte1Symbols = BindingSymbols({ProbeGroup::Mte1});
    EXPECT_EQ(std::find(mte1Symbols.begin(), mte1Symbols.end(), "__sanitizer_report_set_padding"), mte1Symbols.end());

    const auto scalarSymbols = BindingSymbols({ProbeGroup::Scalar});
    ASSERT_FALSE(scalarSymbols.empty());
    EXPECT_NE(
        std::find(scalarSymbols.begin(), scalarSymbols.end(), "__sanitizer_report_set_padding"), scalarSymbols.end());
    EXPECT_NE(
        std::find(scalarSymbols.begin(), scalarSymbols.end(), "__sanitizer_report_set_pad_cnt_nddma"),
        scalarSymbols.end());
    EXPECT_EQ(
        std::find(scalarSymbols.begin(), scalarSymbols.end(), "__sanitizer_report_set_l1_2d_b16"), scalarSymbols.end());
    EXPECT_NE(std::find(symbols.begin(), symbols.end(), "__sanitizer_report_set_l1_2d_b16"), symbols.end());

    EXPECT_EQ(BindingSymbols({ProbeGroup::Matrix}).size(), 16U);
    const auto vectorSymbols = BindingSymbols({ProbeGroup::Vector});
    EXPECT_EQ(vectorSymbols.size(), 10U);
    EXPECT_NE(
        std::find(vectorSymbols.begin(), vectorSymbols.end(), "__sanitizer_report_copy_ubuf_to_ubuf"),
        vectorSymbols.end());
}

TEST(CtrlbinGeneratorTest, IncludesScalarStateBindingsForMte3AndFixpipeConsumers)
{
    const auto mte3Symbols = BindingSymbols({ProbeGroup::Mte3});
    EXPECT_NE(
        std::find(mte3Symbols.begin(), mte3Symbols.end(), "__sanitizer_report_set_loop_size_ubtoout"),
        mte3Symbols.end());

    const auto fixpipeSymbols = BindingSymbols({ProbeGroup::Fixpipe});
    EXPECT_NE(
        std::find(fixpipeSymbols.begin(), fixpipeSymbols.end(), "__sanitizer_report_set_loop3_para"),
        fixpipeSymbols.end());
}

TEST(CtrlbinGeneratorTest, ExposesStableBindingIdentity) { EXPECT_EQ(CtrlBinGeneratorIdentity().size(), 16U); }

TEST(CtrlbinGeneratorTest, GeneratedCatalogMatchesEveryBindingSymbol)
{
    std::vector<std::string> generatedSymbols;
    for (const ProbeGroup group : AllProbeGroups()) {
        const GeneratedProbeSource generated = GenerateProbeSource("dav-3510", group);
        ASSERT_TRUE(generated.success) << generated.diagnostic;
        generatedSymbols.insert(generatedSymbols.end(), generated.symbols.begin(), generated.symbols.end());
    }
    auto bindingSymbols = BindingSymbols(AllProbeGroups());
    std::sort(generatedSymbols.begin(), generatedSymbols.end());
    std::sort(bindingSymbols.begin(), bindingSymbols.end());
    ASSERT_FALSE(bindingSymbols.empty());
    EXPECT_EQ(std::adjacent_find(generatedSymbols.begin(), generatedSymbols.end()), generatedSymbols.end())
        << "duplicate generated probe symbol";
    std::vector<std::string> missing, extra;
    std::set_difference(
        bindingSymbols.begin(), bindingSymbols.end(), generatedSymbols.begin(), generatedSymbols.end(),
        std::back_inserter(missing));
    std::set_difference(
        generatedSymbols.begin(), generatedSymbols.end(), bindingSymbols.begin(), bindingSymbols.end(),
        std::back_inserter(extra));
    EXPECT_TRUE(missing.empty()) << "Missing probe definitions: " << ::testing::PrintToString(missing);
    EXPECT_TRUE(extra.empty()) << "Probe definitions without bindings: " << ::testing::PrintToString(extra);
}

TEST(CtrlbinGeneratorTest, ConcurrentRequestsRemainIsolated)
{
    const auto root = boost::filesystem::temp_directory_path() / "ctrlbin_generator_concurrency";
    boost::filesystem::remove_all(root);
    boost::filesystem::create_directories(root);
    std::string diagnostic;
    ASSERT_TRUE(GenerateCtrlBin((root / "mte2-reference.bin").string(), {ProbeGroup::Mte2}, diagnostic));
    ASSERT_TRUE(GenerateCtrlBin((root / "sync-reference.bin").string(), {ProbeGroup::Sync}, diagnostic));
    const std::string expectedMte2 = ReadGeneratedFile(root / "mte2-reference.bin");
    const std::string expectedSync = ReadGeneratedFile(root / "sync-reference.bin");

    constexpr std::size_t kThreadCount = 24;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<uint8_t> generated(kThreadCount, 0);
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < kThreadCount; ++index) {
        workers.emplace_back([&, index] {
            ++ready;
            while (!start.load()) {
                std::this_thread::yield();
            }
            const bool mte2 = index % 2 == 0;
            std::string error;
            const auto path = root / ("result-" + std::to_string(index) + ".bin");
            if (GenerateCtrlBin(path.string(), {mte2 ? ProbeGroup::Mte2 : ProbeGroup::Sync}, error)) {
                generated[index] = ReadGeneratedFile(path) == (mte2 ? expectedMte2 : expectedSync) ? 1U : 0U;
            }
        });
    }
    while (ready.load() != kThreadCount) {
        std::this_thread::yield();
    }
    start = true;
    for (auto& worker : workers) {
        worker.join();
    }
    for (std::size_t index = 0; index < kThreadCount; ++index) {
        EXPECT_TRUE(generated[index]) << "thread " << index << " produced a mixed control file";
    }
    boost::filesystem::remove_all(root);
}

TEST(ToolRunnerTest, CapturesStdoutStderrAndExitStatus)
{
    const auto result = RunTool({"/bin/sh", "-c", "printf stdout; printf stderr >&2; exit 7"});
    EXPECT_EQ(result.exitCode, 7);
    EXPECT_EQ(result.standardOutput, "stdout");
    EXPECT_EQ(result.standardError, "stderr");
}

TEST(ToolRunnerTest, ReportsExecFailure)
{
    const auto result = RunTool({"/definitely/missing/dbi-tool"});
    EXPECT_NE(result.exitCode, 0);
    EXPECT_FALSE(result.standardError.empty());
}

TEST(ToolRunnerTest, RemovesInjectionEnvironmentFromChild)
{
    ASSERT_EQ(setenv("ACL_API_INJECTION", "/tmp/libnpu_check_processor.so", 1), 0);
    ASSERT_EQ(setenv("LD_PRELOAD", "/lib/x86_64-linux-gnu/libm.so.6", 1), 0);

    const auto result = RunTool({"/bin/sh", "-c", "test -z \"$ACL_API_INJECTION\" && test -z \"$LD_PRELOAD\""});

    EXPECT_EQ(unsetenv("ACL_API_INJECTION"), 0);
    EXPECT_EQ(unsetenv("LD_PRELOAD"), 0);
    EXPECT_EQ(result.exitCode, 0) << result.standardError;
}

} // namespace
} // namespace aclsan
