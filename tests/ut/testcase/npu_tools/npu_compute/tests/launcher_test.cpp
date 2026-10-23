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
    ASSERT_TRUE(results.size() == 4U);
    ASSERT_TRUE(results[0].name == "HardwareInfo.jsonl");
    ASSERT_TRUE(results[1].name == "PipeUtilization.csv");
    ASSERT_TRUE(results[2].name == "summary.db");
    ASSERT_TRUE(results[2].type == npucompute::cli::NpuRepFileType::Sqlite);
    ASSERT_TRUE(results[3].name == "summary.jsonl");
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

} // namespace
