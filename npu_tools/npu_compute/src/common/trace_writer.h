/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMMON_TRACE_WRITER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMMON_TRACE_WRITER_H

#include <boost/filesystem/path.hpp>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>

namespace npucompute {

// Replace preserves the legacy BIU rename policy. NoReplace uses exclusive 0600
// creation and an atomic hard-link publication which never overwrites a target.
enum class TraceFilePolicy { Replace, NoReplace };
class TraceFileOutput {
public:
    explicit TraceFileOutput(
        const boost::filesystem::path& path, TraceFilePolicy policy = TraceFilePolicy::NoReplace,
        const std::string& label = "Communication");
    ~TraceFileOutput();
    TraceFileOutput(const TraceFileOutput&) = delete;
    TraceFileOutput& operator=(const TraceFileOutput&) = delete;

    void Write(const std::string& value);
    void Flush();
    void Close();
    void Publish();

private:
    struct FileCloser {
        void operator()(FILE* file) const;
    };
    boost::filesystem::path target_, temporary_;
    TraceFilePolicy policy_;
    std::string label_;
    std::unique_ptr<FILE, FileCloser> file_;
    bool ownsTemporary_ = false;
};

std::string TraceQuote(const std::string& value);
std::string TraceNumber(double value);
std::string TraceNumber(long double value);

// JSON tokens preserve numeric Communication IDs and legacy string BIU IDs,
// and retain each caller's timestamp precision without a numeric conversion.
struct TraceDurationEvent {
    std::string pidJson;
    std::string tidJson;
    std::string name;
    std::string timestampUsJson;
    std::string durationUsJson;
    std::optional<std::string> color;
    std::optional<std::string> argsJson;
};
struct TraceMetadataEvent {
    std::string pidJson;
    std::string tidJson;
    std::string name;
    std::string field;
    std::string valueJson;
};

enum class TraceLayout { Pipeline, Array };
class TraceWriter {
public:
    TraceWriter(TraceFileOutput& output, TraceLayout layout);
    void Duration(const TraceDurationEvent& event);
    void Metadata(const TraceMetadataEvent& event);
    void Finish();

private:
    void Event(const std::string& event);
    TraceFileOutput& output_;
    TraceLayout layout_;
    bool first_ = true;
};

} // namespace npucompute
#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMMON_TRACE_WRITER_H
