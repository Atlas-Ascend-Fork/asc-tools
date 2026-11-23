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
#include "launch/launcher.h"
#include "launch/process_launcher.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using npucompute::cli::CliConfig;
using npucompute::cli::ImportedProfileEntry;
using npucompute::cli::LaunchTarget;
using npucompute::cli::ReadImportedProfileResults;

class TestDirectory {
public:
    TestDirectory()
    {
        std::string pathTemplate = "/tmp/npu-compute-launcher-test-XXXXXX";
        pathTemplate.push_back('\0');
        char* created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TestDirectory()
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

class ScopedCurrentDirectory {
public:
    explicit ScopedCurrentDirectory(const boost::filesystem::path& path)
    {
        boost::system::error_code error;
        original_ = boost::filesystem::current_path(error);
        if (error) {
            return;
        }
        boost::filesystem::current_path(path, error);
        active_ = !error;
    }

    ~ScopedCurrentDirectory()
    {
        if (active_) {
            boost::system::error_code error;
            boost::filesystem::current_path(original_, error);
        }
    }

    bool IsActive() const { return active_; }

private:
    boost::filesystem::path original_;
    bool active_ = false;
};

class ScopedTmpdir {
public:
    explicit ScopedTmpdir(const std::string& value)
    {
        const char* original = std::getenv("TMPDIR");
        if (original != nullptr) {
            original_ = original;
            hadOriginal_ = true;
        }
        ::setenv("TMPDIR", value.c_str(), 1);
    }

    ~ScopedTmpdir()
    {
        if (hadOriginal_) {
            ::setenv("TMPDIR", original_.c_str(), 1);
        } else {
            ::unsetenv("TMPDIR");
        }
    }

private:
    std::string original_;
    bool hadOriginal_ = false;
};

CliConfig ShellConfig(const std::string& command)
{
    CliConfig config;
    config.sections = {"PipeUtilization"};
    config.program = "/bin/sh";
    config.program_arguments = {"-c", command};
    return config;
}

CliConfig ShellConfig(const std::string& command, const boost::filesystem::path& output)
{
    CliConfig config = ShellConfig(command);
    config.export_path = output.string();
    return config;
}

CliConfig FixtureConfig(const boost::filesystem::path& output = {}, const std::string& mode = {})
{
    CliConfig config;
    config.sections = {"PipeUtilization"};
    config.program = NPU_COMPUTE_REP_FIXTURE_PATH;
    if (!output.empty()) {
        config.export_path = output.string();
    }
    if (!mode.empty()) {
        config.program_arguments = {"--mode", mode};
    }
    return config;
}

bool HasCollectionDirectoryIn(const boost::filesystem::path& parent)
{
    for (const boost::filesystem::directory_entry& entry : boost::filesystem::directory_iterator(parent)) {
        if (boost::filesystem::is_directory(entry.path()) &&
            entry.path().filename().string().find("npu-compute-") == 0U) {
            return true;
        }
    }
    return false;
}

void CheckReportContents(const boost::filesystem::path& report)
{
    std::vector<ImportedProfileEntry> results;
    std::string error;
    ASSERT_TRUE(ReadImportedProfileResults(report, &results, &error));
    ASSERT_TRUE(results.size() == 5U);
    ASSERT_TRUE(results[0].name == "HardwareInfo.jsonl");
    ASSERT_TRUE(results[1].name == "Occupancy.json");
    ASSERT_TRUE(results[1].type == npucompute::cli::NpuRepFileType::Json);
    ASSERT_TRUE(results[2].name == "PipeUtilization.csv");
    ASSERT_TRUE(results[3].name == "summary.db");
    ASSERT_TRUE(results[3].type == npucompute::cli::NpuRepFileType::Sqlite);
    ASSERT_TRUE(results[4].name == "summary.jsonl");
}

TEST(NpuComputeLauncher, ReplacesAllMatchingEnvironmentEntries)
{
    npucompute::cli::ProcessLaunchRequest request;
    request.environment = {"KEEP=first", "MODE=old", "MODE_EXTRA=unchanged", "MODE=duplicate", "LAST=value"};

    request.SetEnvironment("MODE", "new=value");

    EXPECT_EQ(
        request.environment,
        (std::vector<std::string>{"KEEP=first", "MODE_EXTRA=unchanged", "LAST=value", "MODE=new=value"}));
}

TEST(NpuComputeLauncher, AppendsEnvironmentWithEmptyValue)
{
    npucompute::cli::ProcessLaunchRequest request;
    request.SetEnvironment("MODE", "");
    EXPECT_EQ(request.environment, (std::vector<std::string>{"MODE="}));

    request.SetEnvironment("MODE", "replacement");
    EXPECT_EQ(request.environment, (std::vector<std::string>{"MODE=replacement"}));
}

TEST(NpuComputeLauncher, PostReadyFailureAllowsTargetCleanup)
{
    TestDirectory workDirectory;
    ASSERT_FALSE(workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const auto marker = workDirectory.Path() / "cleanup.marker";
    auto config = FixtureConfig({}, "protocol-bad-offset");
    config.program_arguments.insert(config.program_arguments.end(), {"--", marker.string()});
    std::string report;
    std::string error;

    EXPECT_NE(LaunchTarget(config, &report, &error), 0);
    EXPECT_NE(error.find("UDS collection failed"), std::string::npos);
    EXPECT_TRUE(boost::filesystem::is_regular_file(marker));
}

TEST(NpuComputeLauncher, ApplicationRanksPublishIndependentPackedReports)
{
    TestDirectory workDirectory;
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    auto config = FixtureConfig({}, "application-fork");
    config.sections = {"Communication"};
    config.replay_mode = npucompute::cli::ReplayMode::Application;

    std::string report;
    std::string error;
    ASSERT_EQ(LaunchTarget(config, &report, &error), 0) << error;
    std::vector<ImportedProfileEntry> results;
    ASSERT_TRUE(ReadImportedProfileResults(report, &results, &error)) << error;
    ASSERT_EQ(results.size(), 2U);
    for (const auto& rank : results) {
        EXPECT_EQ(rank.name.find("pid-"), 0U);
        EXPECT_EQ(rank.children.size(), 2U);
    }
    EXPECT_NE(results[0].name, results[1].name);
    EXPECT_FALSE(HasCollectionDirectoryIn(workDirectory.Path()));
}

TEST(NpuComputeLauncher, ApplicationPreservesCommittedArtifactsAndTargetFailure)
{
    TestDirectory workDirectory;
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    auto config = FixtureConfig({}, "application-abort");
    config.sections = {"Communication"};
    config.replay_mode = npucompute::cli::ReplayMode::Application;
    config.no_pack = true;

    std::string report;
    std::string error;
    ASSERT_EQ(LaunchTarget(config, &report, &error), 23) << error;
    EXPECT_NE(error.find("status 23"), std::string::npos);
    ASSERT_FALSE(report.empty());
    unsigned rankCount = 0;
    unsigned completed = 0;
    for (const auto& rank : boost::filesystem::directory_iterator(report)) {
        EXPECT_EQ(rank.path().filename().string().find("pid-"), 0U);
        EXPECT_TRUE(boost::filesystem::is_regular_file(rank.path() / "HardwareInfo.jsonl"));
        completed += boost::filesystem::is_regular_file(rank.path() / "CommunicationTrace.json");
        ++rankCount;
    }
    EXPECT_EQ(rankCount, 2U);
    EXPECT_EQ(completed, 1U);
    for (const auto& file : boost::filesystem::recursive_directory_iterator(report)) {
        EXPECT_NE(file.path().extension(), ".tmp");
    }
}

TEST(NpuComputeLauncher, SingleApplicationRankKeepsFlatReport)
{
    TestDirectory workDirectory;
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    auto config = FixtureConfig({}, "application-single");
    config.sections = {"Communication"};
    config.replay_mode = npucompute::cli::ReplayMode::Application;
    config.no_pack = true;

    std::string report;
    std::string error;
    ASSERT_EQ(LaunchTarget(config, &report, &error), 0) << error;
    const boost::filesystem::path reportPath(report);
    EXPECT_FALSE(boost::filesystem::exists(reportPath / "application_manifest.json"));
    EXPECT_FALSE(boost::filesystem::exists(reportPath / "application_raw.jsonl"));
    EXPECT_TRUE(boost::filesystem::is_regular_file(reportPath / "CommunicationTrace.json"));
    for (const auto& entry : boost::filesystem::directory_iterator(reportPath)) {
        EXPECT_NE(entry.path().filename().string().rfind("pid-", 0), 0U);
    }
}

TEST(NpuComputeLauncher, WithoutExportPublishesReportInCurrentDirectory)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const CliConfig config = FixtureConfig();
    std::string report;
    std::string error = "old error";

    const int launchResult = LaunchTarget(config, &report, &error);
    if (launchResult != 0) {
        std::fprintf(stderr, "launch failed (%d): %s\n", launchResult, error.c_str());
    }
    ASSERT_TRUE(launchResult == 0);
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(boost::filesystem::path(report).parent_path() == workDirectory.Path());
    ASSERT_TRUE(boost::filesystem::path(report).extension() == ".npu-rep");
    ASSERT_TRUE(boost::filesystem::is_regular_file(report));
    ASSERT_NO_FATAL_FAILURE(CheckReportContents(report));
}

TEST(NpuComputeLauncher, ExplicitReportRemovesDataFromCurrentDirectory)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const boost::filesystem::path output = workDirectory.Path() / "result.npu-rep";
    const CliConfig config = FixtureConfig(output);
    std::string report;
    std::string error;

    ASSERT_TRUE(LaunchTarget(config, &report, &error) == 0);
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(report == output.string());
    ASSERT_TRUE(boost::filesystem::is_regular_file(output));
    ASSERT_NO_FATAL_FAILURE(CheckReportContents(output));
}

TEST(NpuComputeLauncher, ExportDirectoryRemovesCollectionData)
{
    TestDirectory workDirectory;
    TestDirectory reportDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    ASSERT_TRUE(!reportDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const CliConfig config = FixtureConfig(reportDirectory.Path());
    std::string report;
    std::string error;

    ASSERT_TRUE(LaunchTarget(config, &report, &error) == 0);
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(boost::filesystem::path(report).parent_path() == reportDirectory.Path());
    ASSERT_TRUE(boost::filesystem::is_regular_file(report));
}

TEST(NpuComputeLauncher, InvalidTmpdirDoesNotAffectCollection)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const ScopedTmpdir tmpdir((workDirectory.Path() / "missing-tmpdir").string());
    const CliConfig config = FixtureConfig(workDirectory.Path() / "result.npu-rep");
    std::string report;
    std::string error;

    ASSERT_TRUE(LaunchTarget(config, &report, &error) == 0);
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(boost::filesystem::is_regular_file(report));
}

TEST(NpuComputeLauncher, InvalidExportIsRejectedBeforeAppLaunch)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const boost::filesystem::path marker = workDirectory.Path() / "app-ran";
    const boost::filesystem::path output = workDirectory.Path() / "missing" / "result.npu-rep";
    const CliConfig config = ShellConfig("touch app-ran", output);
    std::string report = "old report";
    std::string error;

    ASSERT_TRUE(LaunchTarget(config, &report, &error) == 4);
    ASSERT_TRUE(report.empty());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(!boost::filesystem::exists(marker));
    ASSERT_TRUE(!boost::filesystem::exists(output));
    ASSERT_TRUE(error.find("Create it before using --export") != std::string::npos);
}

TEST(NpuComputeLauncher, FailedAppRemovesEmptyDataAndDoesNotPublishReport)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const boost::filesystem::path output = workDirectory.Path() / "result.npu-rep";
    CliConfig config = FixtureConfig(output);
    config.program_arguments = {"--exit-code", "23"};
    std::string report = "old report";
    std::string error;

    ASSERT_TRUE(LaunchTarget(config, &report, &error) == 23);
    ASSERT_TRUE(report.empty());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(boost::filesystem::is_empty(workDirectory.Path()));
    ASSERT_TRUE(!boost::filesystem::exists(output));
    ASSERT_TRUE(error.find("APP exited with status 23") != std::string::npos);
}

TEST(NpuComputeLauncher, MissingHardwareInfoRemovesEmptyDataAndDoesNotPublishReport)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const boost::filesystem::path output = workDirectory.Path() / "result.npu-rep";
    const CliConfig config = FixtureConfig(output, "empty");
    std::string report;
    std::string error;

    ASSERT_TRUE(LaunchTarget(config, &report, &error) == 3);
    ASSERT_TRUE(report.empty());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(boost::filesystem::is_empty(workDirectory.Path()));
    ASSERT_TRUE(!boost::filesystem::exists(output));
    ASSERT_TRUE(error.find("required artifact missing") != std::string::npos);
}

TEST(NpuComputeLauncher, MissingHardwareInfoPublishesPartialReport)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const boost::filesystem::path output = workDirectory.Path() / "result.npu-rep";
    const CliConfig config = FixtureConfig(output, "missing-hardware");
    std::string report;
    std::string error;

    ASSERT_TRUE(LaunchTarget(config, &report, &error) == 3);
    ASSERT_TRUE(report == output.string());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(boost::filesystem::is_regular_file(output));
    ASSERT_TRUE(error.find("collection incomplete") != std::string::npos);
}

TEST(NpuComputeLauncher, MissingOrEmptyOccupancyPublishesOtherReportsSuccessfully)
{
    for (const auto* mode : {"missing-occupancy", "empty-occupancy"}) {
        SCOPED_TRACE(mode);
        TestDirectory workDirectory;
        ASSERT_TRUE(!workDirectory.Path().empty());
        const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
        ASSERT_TRUE(currentDirectory.IsActive());
        const auto output = workDirectory.Path() / "result.npu-rep";
        const auto config = FixtureConfig(output, mode);
        std::string report;
        std::string error;

        EXPECT_EQ(LaunchTarget(config, &report, &error), 0) << error;
        std::vector<ImportedProfileEntry> results;
        ASSERT_TRUE(ReadImportedProfileResults(output, &results, &error)) << error;
        ASSERT_EQ(results.size(), 4U);
        EXPECT_EQ(results[0].name, "HardwareInfo.jsonl");
        EXPECT_EQ(results[1].name, "PipeUtilization.csv");
        EXPECT_EQ(results[2].name, "summary.db");
        EXPECT_EQ(results[3].name, "summary.jsonl");
    }
}

TEST(NpuComputeLauncher, IncompleteResultPublishesPartialReport)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const boost::filesystem::path output = workDirectory.Path() / "result.npu-rep";
    const CliConfig config = FixtureConfig(output, "incomplete");
    std::string report;
    std::string error;

    ASSERT_TRUE(LaunchTarget(config, &report, &error) == 3);
    ASSERT_TRUE(report == output.string());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(boost::filesystem::is_regular_file(output));
    ASSERT_TRUE(error.find("collection incomplete") != std::string::npos);
}

TEST(NpuComputeLauncher, MissingWarpPreservesSuccessfulReports)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const auto output = workDirectory.Path() / "result.npu-rep";
    CliConfig config = FixtureConfig(output, "missing-warp");
    config.sections.push_back("occupancy");
    config.sections.push_back("Pipeline");
    config.collect_pipeline = true;
    std::string report;
    std::string error;

    EXPECT_EQ(LaunchTarget(config, &report, &error), 0) << error;
    ASSERT_EQ(report, output.string());
    std::vector<ImportedProfileEntry> results;
    ASSERT_TRUE(ReadImportedProfileResults(output, &results, &error)) << error;
    const auto hasEntry = [&](const std::string& name) {
        return std::any_of(results.begin(), results.end(), [&](const auto& result) { return result.name == name; });
    };
    EXPECT_TRUE(hasEntry("PipeUtilization.csv"));
    EXPECT_TRUE(hasEntry("PipeTrace.json"));
    EXPECT_TRUE(hasEntry("summary.jsonl"));
    EXPECT_FALSE(hasEntry("SimtWarpOccupancy.json"));
}

TEST(NpuComputeLauncher, PublishingFailureRemovesCollectionDataDirectory)
{
    TestDirectory workDirectory;
    ASSERT_TRUE(!workDirectory.Path().empty());
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    const boost::filesystem::path output =
        boost::filesystem::path("/proc") / ("npu-compute-launcher-" + std::to_string(::getpid()) + ".npu-rep");
    const CliConfig config = FixtureConfig(output);
    std::string report;
    std::string error;

    ASSERT_TRUE(LaunchTarget(config, &report, &error) == 4);
    ASSERT_TRUE(report.empty());
    ASSERT_TRUE(!HasCollectionDirectoryIn(workDirectory.Path()));
    ASSERT_TRUE(!boost::filesystem::exists(output));
    ASSERT_TRUE(error.find("create temporary rep failed") != std::string::npos);
    ASSERT_TRUE(error.find("/proc/") != std::string::npos);
}

TEST(NpuComputeLauncher, NoPackPreservesDirectoryAndCleansFailures)
{
    TestDirectory workDirectory;
    TestDirectory outputDirectory;
    const ScopedCurrentDirectory currentDirectory(workDirectory.Path());
    ASSERT_TRUE(currentDirectory.IsActive());
    CliConfig config = FixtureConfig(outputDirectory.Path());
    config.no_pack = true;
    std::string output;
    std::string error;
    ASSERT_TRUE(LaunchTarget(config, &output, &error) == 0);
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(boost::filesystem::is_directory(output));
    ASSERT_TRUE(boost::filesystem::path(output).parent_path() == outputDirectory.Path());
    ASSERT_TRUE(boost::filesystem::exists(boost::filesystem::path(output) / "PipeUtilization.csv"));
    ASSERT_TRUE(!boost::filesystem::exists(boost::filesystem::path(output) / ".hardware_info.lock"));
    ASSERT_TRUE(boost::filesystem::is_empty(workDirectory.Path()));
    boost::filesystem::remove_all(output);
    config.program_arguments = {"--exit-code", "23"};
    ASSERT_TRUE(LaunchTarget(config, &output, &error) == 23);
    ASSERT_TRUE(output.empty());
    ASSERT_TRUE(boost::filesystem::is_empty(outputDirectory.Path()));
    config = ShellConfig("touch app-ran", outputDirectory.Path() / "custom.npu-rep");
    config.no_pack = true;
    ASSERT_TRUE(LaunchTarget(config, &output, &error) == 4);
    ASSERT_TRUE(!boost::filesystem::exists(workDirectory.Path() / "app-ran"));
}

} // namespace
