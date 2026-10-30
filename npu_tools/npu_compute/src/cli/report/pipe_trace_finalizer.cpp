/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/pipe_trace_finalizer.h"

#include <limits>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <unistd.h>

namespace npucompute::cli {
namespace {

struct TraceEvent {
    std::string color;
    double duration = 0.0;
    std::string name;
    std::string phase;
    std::string pid;
    std::string tid;
    double timestamp = 0.0;
};

struct Fragment {
    std::string processName;
    std::size_t processOrdinal = 0;
    uint64_t resultSequence = 0;
    uint64_t replayId = 0;
    int32_t deviceId = -1;
    std::string file;
    uint64_t expectedEventCount = 0;
    std::vector<TraceEvent> events;
};

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool ParseUint64(const std::string& value, uint64_t* result)
{
    if (value.empty() || result == nullptr || value.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size()) {
            return false;
        }
        *result = static_cast<uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadJson(const boost::filesystem::path& path, boost::property_tree::ptree* root, std::string* error)
{
    try {
        boost::property_tree::read_json(path.string(), *root);
        return true;
    } catch (const std::exception& exception) {
        return Fail("parse JSON failed: " + path.string() + ": " + exception.what(), error);
    }
}

bool IsSafeFragmentName(const std::string& name)
{
    const boost::filesystem::path path(name);
    return !name.empty() && path == path.filename() && name != "." && name != ".." && name[0] != '.';
}

bool ReadFragment(const boost::filesystem::path& path, Fragment* fragment, std::string* error)
{
    boost::property_tree::ptree root;
    if (!ReadJson(path, &root, error)) {
        return false;
    }
    try {
        if (root.get<std::string>("displayTimeUnit") != "ns" || root.get<std::string>("profilingType") != "op" ||
            root.get<int>("schemaVersion") != 1) {
            return Fail("PipeTrace fragment header is invalid: " + path.string(), error);
        }
        for (const auto& item : root.get_child("traceEvents")) {
            const auto& node = item.second;
            TraceEvent event;
            event.color = node.get<std::string>("cname");
            event.duration = node.get<double>("dur");
            event.name = node.get<std::string>("name");
            event.phase = node.get<std::string>("ph");
            event.pid = node.get<std::string>("pid");
            event.tid = node.get<std::string>("tid");
            event.timestamp = node.get<double>("ts");
            if (event.phase != "X" || event.pid.empty() || event.name.empty() || event.tid.empty() ||
                !std::isfinite(event.duration) || event.duration < 0.0 || !std::isfinite(event.timestamp) ||
                event.timestamp < 0.0) {
                return Fail("PipeTrace fragment event is invalid: " + path.string(), error);
            }
            fragment->events.push_back(std::move(event));
        }
    } catch (const std::exception& exception) {
        return Fail("PipeTrace fragment is incomplete: " + path.string() + ": " + exception.what(), error);
    }
    if (fragment->events.size() != fragment->expectedEventCount) {
        return Fail("PipeTrace fragment event count mismatch: " + path.string(), error);
    }
    return true;
}

bool ReadProcessManifest(
    const boost::filesystem::path& processDirectory, const std::string& processName, std::size_t processOrdinal,
    std::vector<Fragment>* fragments, std::string* error, std::vector<std::string>* issues)
{
    boost::property_tree::ptree root;
    if (!ReadJson(processDirectory / "manifest.json", &root, error)) {
        return false;
    }
    try {
        const auto state = root.get<std::string>("state");
        const auto status = root.get<int>("status");
        if (root.get<int>("version") != 1 || (state != "complete" && state != "failed")) {
            return Fail("BIU manifest is not complete: " + processDirectory.string(), error);
        }
        if ((state == "failed" || status != 0) && issues != nullptr) {
            issues->push_back("BIU collection failed: " + processDirectory.string());
        }
        for (const auto& item : root.get_child("fragments")) {
            Fragment fragment;
            fragment.processName = processName;
            fragment.processOrdinal = processOrdinal;
            if (!ParseUint64(item.second.get<std::string>("resultSequence"), &fragment.resultSequence) ||
                !ParseUint64(item.second.get<std::string>("replayId"), &fragment.replayId) ||
                !ParseUint64(item.second.get<std::string>("eventCount"), &fragment.expectedEventCount)) {
                return Fail("BIU manifest contains an invalid 64-bit field: " + processDirectory.string(), error);
            }
            fragment.deviceId = item.second.get<int32_t>("deviceId");
            fragment.file = item.second.get<std::string>("file");
            if (fragment.deviceId < 0 || !IsSafeFragmentName(fragment.file)) {
                return Fail("BIU manifest contains an invalid fragment source: " + processDirectory.string(), error);
            }
            fragments->push_back(std::move(fragment));
        }
    } catch (const std::exception& exception) {
        return Fail("BIU manifest is incomplete: " + processDirectory.string() + ": " + exception.what(), error);
    }
    return true;
}

bool ValidateProcessFiles(
    const boost::filesystem::path& processDirectory, const std::string& processName,
    const std::vector<Fragment>& fragments, std::string* error, std::vector<std::string>* issues)
{
    std::set<std::string> allowed{"manifest.json"};
    for (const Fragment& fragment : fragments) {
        if (fragment.processName == processName && !allowed.insert(fragment.file).second) {
            return Fail("BIU manifest lists a fragment more than once: " + fragment.file, error);
        }
    }
    boost::system::error_code directoryError;
    boost::filesystem::directory_iterator iterator(processDirectory, directoryError), end;
    if (directoryError) {
        return Fail("open BIU process directory failed: " + processDirectory.string(), error);
    }
    for (; iterator != end && !directoryError; iterator.increment(directoryError)) {
        boost::system::error_code itemError;
        const auto status = boost::filesystem::symlink_status(iterator->path(), itemError);
        const std::string name = iterator->path().filename().string();
        if (itemError || boost::filesystem::is_symlink(status) || !boost::filesystem::is_regular_file(status) ||
            allowed.erase(name) == 0) {
            if (issues != nullptr) {
                issues->push_back("unlisted or invalid BIU process file: " + iterator->path().string());
            }
        }
    }
    if (directoryError) {
        return Fail("iterate BIU process directory failed: " + processDirectory.string(), error);
    }
    return true; // Listed fragments are checked individually before merging.
}

std::string EscapeJson(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value) {
        switch (character) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += character;
                break;
        }
    }
    return escaped;
}

bool WriteFinalTrace(
    const boost::filesystem::path& collectionDirectory, const std::vector<Fragment>& fragments, std::string* error)
{
    const boost::filesystem::path outputPath = collectionDirectory / "PipeTrace.json";
    boost::system::error_code existsError;
    const bool outputExists = boost::filesystem::exists(outputPath, existsError);
    if (existsError && existsError.value() != ENOENT) {
        return Fail("inspect PipeTrace.json failed: " + existsError.message(), error);
    }
    if (outputExists) {
        return Fail("PipeTrace.json already exists", error);
    }
    const boost::filesystem::path temporaryPath =
        collectionDirectory / ("PipeTrace.json.tmp." + std::to_string(static_cast<long long>(::getpid())));
    std::ofstream output(temporaryPath.string(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return Fail("open final PipeTrace temporary file failed", error);
    }
    output << "{\"displayTimeUnit\":\"ns\",\"profilingType\":\"op\",\"schemaVersion\":1,\"traceEvents\":[";
    bool first = true;
    const bool qualifyPid = fragments.size() > 1U;
    for (const Fragment& fragment : fragments) {
        for (const TraceEvent& event : fragment.events) {
            std::string pid = event.pid;
            if (qualifyPid) {
                pid = "process" + std::to_string(fragment.processOrdinal) + ".result" +
                      std::to_string(fragment.resultSequence) + ".device" + std::to_string(fragment.deviceId) +
                      ".replay" + std::to_string(fragment.replayId) + "." + pid;
            }
            output << (first ? "" : ",") << "{\"cname\":\"" << EscapeJson(event.color)
                   << "\",\"dur\":" << std::setprecision(std::numeric_limits<double>::max_digits10) << event.duration
                   << ",\"name\":\"" << EscapeJson(event.name) << "\",\"ph\":\"X\",\"pid\":\"" << EscapeJson(pid)
                   << "\",\"tid\":\"" << EscapeJson(event.tid)
                   << "\",\"ts\":" << std::setprecision(std::numeric_limits<double>::max_digits10) << event.timestamp
                   << "}";
            first = false;
        }
    }
    output << "]}\n";
    output.flush();
    output.close();
    if (output.fail()) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporaryPath, ignored);
        return Fail("write final PipeTrace failed", error);
    }
    boost::system::error_code renameError;
    boost::filesystem::rename(temporaryPath, outputPath, renameError);
    if (renameError) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporaryPath, ignored);
        return Fail("commit final PipeTrace failed: " + renameError.message(), error);
    }
    return true;
}

bool CollectProcessDirectories(
    const boost::filesystem::path& stagingRoot, std::vector<boost::filesystem::path>* processDirectories,
    std::vector<std::string>* issues, std::string* error)
{
    boost::system::error_code statusError;
    boost::filesystem::directory_iterator iterator(stagingRoot, statusError), end;
    if (statusError) {
        return Fail("open BIU staging failed: " + statusError.message(), error);
    }
    for (; iterator != end && !statusError; iterator.increment(statusError)) {
        boost::system::error_code itemError;
        const auto status = boost::filesystem::symlink_status(iterator->path(), itemError);
        if (itemError || boost::filesystem::is_symlink(status) || !boost::filesystem::is_directory(status)) {
            if (issues != nullptr) {
                issues->push_back("invalid BIU staging item: " + iterator->path().string());
            }
            continue;
        }
        processDirectories->push_back(iterator->path());
    }
    if (statusError) {
        return Fail("iterate BIU staging failed: " + statusError.message(), error);
    }
    std::sort(processDirectories->begin(), processDirectories->end());
    return true;
}

bool ReadStagedFragments(
    const std::vector<boost::filesystem::path>& processDirectories, std::vector<Fragment>* fragments,
    std::vector<std::string>* issues, std::string* error)
{
    for (std::size_t index = 0; index < processDirectories.size(); ++index) {
        const std::size_t start = fragments->size();
        std::string processError;
        const std::string processName = processDirectories[index].filename().string();
        if (!ReadProcessManifest(processDirectories[index], processName, index, fragments, &processError, issues) ||
            !ValidateProcessFiles(processDirectories[index], processName, *fragments, &processError, issues)) {
            fragments->resize(start);
            if (issues != nullptr) {
                issues->push_back(processError);
            }
        }
    }
    std::sort(fragments->begin(), fragments->end(), [](const Fragment& left, const Fragment& right) {
        return std::tie(left.processName, left.resultSequence, left.replayId, left.deviceId) <
               std::tie(right.processName, right.resultSequence, right.replayId, right.deviceId);
    });
    return !fragments->empty() || Fail("BIU manifests contain no completed fragment", error);
}

bool ValidateStagedFragments(
    const boost::filesystem::path& stagingRoot, std::vector<Fragment>* fragments, std::vector<std::string>* issues,
    std::string* error)
{
    fragments->erase(
        std::remove_if(
            fragments->begin(), fragments->end(),
            [&](Fragment& fragment) {
                const boost::filesystem::path processDirectory = stagingRoot / fragment.processName;
                const boost::filesystem::path fragmentPath = processDirectory / fragment.file;
                boost::system::error_code itemError;
                const auto status = boost::filesystem::symlink_status(fragmentPath, itemError);
                std::string fragmentError;
                if (itemError || boost::filesystem::is_symlink(status) || !boost::filesystem::is_regular_file(status)) {
                    fragmentError = "invalid BIU fragment: " + fragmentPath.string();
                } else if (ReadFragment(fragmentPath, &fragment, &fragmentError)) {
                    return false;
                }
                if (issues != nullptr) {
                    issues->push_back(fragmentError);
                }
                return true;
            }),
        fragments->end());
    return !fragments->empty() || Fail("BIU manifests contain no valid fragment", error);
}

} // namespace

bool FinalizePipeTrace(
    const boost::filesystem::path& collectionDirectory, bool pipelineEnabled, std::string* error,
    std::vector<std::string>* issues)
{
    if (error != nullptr) {
        error->clear();
    }
    const boost::filesystem::path stagingRoot = collectionDirectory / ".biu-staging";
    boost::system::error_code statusError;
    const bool stagingExists = boost::filesystem::exists(stagingRoot, statusError);
    if (statusError && statusError.value() != ENOENT) {
        return Fail("inspect BIU staging failed: " + statusError.message(), error);
    }
    if (!pipelineEnabled) {
        return stagingExists ? Fail("unexpected BIU staging exists while pipeline is disabled", error) : true;
    }
    const auto stagingStatus = boost::filesystem::symlink_status(stagingRoot, statusError);
    if (!stagingExists || statusError || !boost::filesystem::is_directory(stagingStatus)) {
        return Fail("BIU staging directory is missing", error);
    }
    std::vector<boost::filesystem::path> processDirectories;
    if (!CollectProcessDirectories(stagingRoot, &processDirectories, issues, error)) {
        return false;
    }
    if (processDirectories.empty()) {
        return Fail("BIU staging contains no process result", error);
    }
    std::vector<Fragment> fragments;
    if (!ReadStagedFragments(processDirectories, &fragments, issues, error) ||
        !ValidateStagedFragments(stagingRoot, &fragments, issues, error)) {
        return false;
    }
    if (!WriteFinalTrace(collectionDirectory, fragments, error)) {
        return false;
    }
    boost::system::error_code removeError;
    boost::filesystem::remove_all(stagingRoot, removeError);
    if (removeError) {
        return Fail("remove BIU staging failed: " + removeError.message(), error);
    }
    return true;
}

} // namespace npucompute::cli
