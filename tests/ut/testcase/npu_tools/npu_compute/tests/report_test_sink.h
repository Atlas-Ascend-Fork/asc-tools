/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_REPORT_TEST_SINK_H
#define NPU_COMPUTE_REPORT_TEST_SINK_H

#include "artifact_sink.h"
#include "report/report_writer.h"

#include <boost/filesystem.hpp>

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace npucompute::test {

struct ReportFileConfig : ReportConfig {
    std::string outputDirectory;
};

class ReportFileSink final : public ArtifactSink {
public:
    ReportFileSink(const std::string& root, bool avoidOverwrite) : root_(root), avoidOverwrite_(avoidOverwrite) {}

    void Begin(const std::string& name) override
    {
        if (root_.empty() || !boost::filesystem::path(root_).is_absolute()) {
            throw std::runtime_error("test artifact root must be an absolute path");
        }
        if (file_.is_open()) {
            throw std::runtime_error("test artifact begin while another artifact is open");
        }
        if (writeRoot_.empty()) {
            writeRoot_ = boost::filesystem::path(root_);
            if (avoidOverwrite_ && HasExistingArtifact(writeRoot_)) {
                const auto existingCollection = ExistingCollectionDirectory(writeRoot_);
                writeRoot_ = existingCollection.empty() ?
                                 writeRoot_ / boost::filesystem::unique_path("collection-p%%%%-%%%%") :
                                 existingCollection;
            }
            boost::filesystem::create_directories(writeRoot_);
        }
        currentPath_ = writeRoot_ / name;
        boost::filesystem::create_directories(currentPath_.parent_path());
        file_.open(currentPath_.string(), std::ios::binary | std::ios::trunc);
        if (!file_.is_open()) {
            throw std::runtime_error("open test artifact failed");
        }
    }

    void Write(std::string_view bytes) override
    {
        file_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file_.good()) {
            throw std::runtime_error("write test artifact failed");
        }
    }

    void Commit(std::uint64_t) override
    {
        file_.flush();
        file_.close();
        if (file_.fail()) {
            throw std::runtime_error("close test artifact failed");
        }
    }

    void Abort() noexcept override
    {
        file_.close();
        if (!currentPath_.empty()) {
            boost::system::error_code ignored;
            boost::filesystem::remove(currentPath_, ignored);
        }
        currentPath_.clear();
    }

private:
    static bool HasExistingArtifact(const boost::filesystem::path& root)
    {
        if (boost::filesystem::exists(root / "summary.jsonl")) {
            return true;
        }
        for (const auto& section :
             {"L2Cache", "Memory", "MemoryL0", "MemoryUB", "PipeUtilization", "ArithmeticUtilization",
              "ResourceConflictRatio"}) {
            if (boost::filesystem::exists(root / (std::string(section) + ".csv"))) {
                return true;
            }
        }
        return false;
    }

    static boost::filesystem::path ExistingCollectionDirectory(const boost::filesystem::path& root)
    {
        if (!boost::filesystem::is_directory(root)) {
            return {};
        }
        for (const auto& entry : boost::filesystem::directory_iterator(root)) {
            if (boost::filesystem::is_directory(entry) && HasExistingArtifact(entry.path())) {
                return entry.path();
            }
        }
        return {};
    }

    std::string root_;
    bool avoidOverwrite_ = false;
    boost::filesystem::path writeRoot_;
    boost::filesystem::path currentPath_;
    std::ofstream file_;
};

} // namespace npucompute::test

namespace npucompute {

inline aclptiResult WritePmuCsv(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections,
    const test::ReportFileConfig& config, std::string_view artifactPrefix = {}, bool* degraded = nullptr)
{
    test::ReportFileSink sink(config.outputDirectory, true);
    return WritePmuCsv(result, sections, config, sink, artifactPrefix, degraded);
}

inline aclptiResult WriteSummaryJsonl(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections,
    const test::ReportFileConfig& config, const KernelMetadata& metadata, std::string_view artifactPrefix = {})
{
    test::ReportFileSink sink(config.outputDirectory, false);
    return WriteSummaryJsonl(result, sections, config, metadata, sink, artifactPrefix);
}

inline aclptiResult WritePmuReport(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections,
    const test::ReportFileConfig& config, const KernelMetadata& metadata = {}, std::string_view artifactPrefix = {},
    bool* degraded = nullptr)
{
    test::ReportFileSink sink(config.outputDirectory, true);
    return WritePmuReport(result, sections, config, metadata, sink, artifactPrefix, degraded);
}

} // namespace npucompute

#endif
