/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "artifact_receiver.h"
#include "report/pm_sampling_file_validator.h"
#include <algorithm>
#include <boost/filesystem.hpp>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <map>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace npucompute::cli {
namespace {
constexpr std::size_t MAX_ARTIFACT_COUNT = 64;

void Require(bool condition, const char* message)
{
    if (!condition) {
        throw ipc::Exception({1, message, "ARTIFACT", "receive"});
    }
}
void FileError(const char* operation)
{
    throw ipc::Exception(
        {static_cast<uint32_t>(errno), std::string(operation) + ": " + std::strerror(errno), "FILESYSTEM", "artifact"});
}
int OpenOrCreateDirectoryAt(int parent, const std::string& name)
{
    if (mkdirat(parent, name.c_str(), (S_IRWXU)) != 0 && errno != EEXIST) {
        FileError("create artifact directory");
    }
    const int descriptor = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        FileError("open artifact directory");
    }
    return descriptor;
}
void RenameNoReplace(const boost::filesystem::path& directory, const std::string& source, const std::string& target)
{
    const auto sourcePath = directory / source;
    const auto targetPath = directory / target;
    boost::system::error_code error;
    const auto status = boost::filesystem::symlink_status(targetPath, error);
    if (status.type() == boost::filesystem::file_not_found) {
        error.clear();
    } else if (error) {
        errno = error.value();
        FileError("inspect artifact target");
    }
    if (boost::filesystem::exists(status)) {
        errno = EEXIST;
        FileError("publish artifact");
    }
    boost::filesystem::rename(sourcePath, targetPath, error);
    if (error) {
        errno = error.value();
        FileError("publish artifact");
    }
}
bool HasOnlySimdVectorCode(const boost::filesystem::path& disassembly)
{
    std::ifstream input(disassembly.string());
    // Inspect instruction lines only: source text and symbol names are not evidence
    // that a kernel contains a SIMT launch.
    const std::regex vectorInstruction(R"(^\s*[0-9a-fA-F]+:\s+[0-9a-fA-F]+\s+(VF_SIMT|VF)\s)");
    bool simd = false;
    std::string line;
    while (std::getline(input, line)) {
        std::smatch match;
        if (!std::regex_search(line, match, vectorInstruction)) {
            continue;
        }
        if (match[1] == "VF_SIMT") {
            return false;
        }
        simd = true;
    }
    return input.eof() && simd;
}
} // namespace
ArtifactReceiver::ArtifactReceiver(const std::string& directory)
    : directory_path_(boost::filesystem::absolute(directory))
{
    directory_ = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_ < 0) {
        FileError("open staging");
    }
}
ArtifactReceiver::~ArtifactReceiver()
{
    Abort();
    if (directory_ >= 0) {
        close(directory_);
    }
}
void ArtifactReceiver::Abort() noexcept
{
    if (file_ >= 0) {
        close(file_);
        file_ = -1;
    }
    if (!temporary_.empty()) {
        unlinkat(active_directory_ >= 0 ? active_directory_ : directory_, temporary_.c_str(), 0);
        temporary_.clear();
    }
    if (active_directory_ >= 0) {
        close(active_directory_);
        active_directory_ = -1;
    }
    active_name_.clear();
}
void ArtifactReceiver::Receive(const ipc::Frame& frame)
{
    try {
        if (frame.type == ipc::Type::DataBegin) {
            Require(file_ < 0, "ARTIFACT: interleaved begin");
            auto begin = ipc::DecodeArtifactBegin(frame.payload);
            Require(
                begin.id != 0 && begin.kind == ipc::KindForName(begin.name) &&
                    begin.artifactType == ipc::ArtifactTypeForName(begin.name),
                "ARTIFACT: invalid name or kind");
            for (const auto& entry : manifest_) {
                Require(entry.id != begin.id && entry.name != begin.name, "ARTIFACT: duplicate artifact");
            }
            Require(manifest_.size() < MAX_ARTIFACT_COUNT, "ARTIFACT: too many artifacts");
            active_ = std::move(begin);
            const auto separator = active_.name.find_last_of('/');
            active_name_ = separator == std::string::npos ? active_.name : active_.name.substr(separator + 1);
            active_directory_path_ = directory_path_;
            active_directory_ = dup(directory_);
            if (active_directory_ < 0) {
                FileError("duplicate staging descriptor");
            }
            if (separator != std::string::npos) {
                const std::string parent = active_.name.substr(0, separator);
                if (parent.rfind("collection-p", 0) == 0) {
                    active_directory_path_ /= parent;
                    const int next = OpenOrCreateDirectoryAt(directory_, parent);
                    close(active_directory_);
                    active_directory_ = next;
                } else {
                    active_directory_path_ /= ".biu-staging/process-uds";
                    const int staging = OpenOrCreateDirectoryAt(directory_, ".biu-staging");
                    try {
                        const int process = OpenOrCreateDirectoryAt(staging, "process-uds");
                        close(staging);
                        close(active_directory_);
                        active_directory_ = process;
                    } catch (...) {
                        close(staging);
                        throw;
                    }
                }
            }
            temporary_ = "." + active_name_ + "." + std::to_string(active_.id) + ".tmp";
            file_ = openat(
                active_directory_, temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                (S_IRUSR | S_IWUSR));
            if (file_ < 0) {
                temporary_.clear();
                FileError("create artifact");
            }
            offset_ = 0;
            return;
        }
        Require(file_ >= 0, "ARTIFACT: data without begin");
        if (frame.type == ipc::Type::Pmu || frame.type == ipc::Type::Pipeline || frame.type == ipc::Type::Hardware ||
            frame.type == ipc::Type::PmSampling) {
            Require(frame.type == ipc::TypeForKind(active_.kind), "ARTIFACT: data kind mismatch");
            auto chunk = ipc::DecodeArtifactChunk(frame.payload);
            Require(chunk.id == active_.id && chunk.offset == offset_, "ARTIFACT: invalid offset or id");
            size_t done = 0;
            while (done < chunk.data.size()) {
                const ssize_t count = write(file_, chunk.data.data() + done, chunk.data.size() - done);
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count <= 0) {
                    FileError("write artifact");
                }
                done += static_cast<size_t>(count);
            }
            offset_ += done;
        } else if (frame.type == ipc::Type::DataEnd) {
            auto end = ipc::DecodeArtifactEnd(frame.payload);
            Require(end.id == active_.id && end.bytes == offset_, "ARTIFACT: invalid end length");
            if (fsync(file_) != 0) {
                FileError("fsync artifact");
            }
            if (close(file_) != 0) {
                file_ = -1;
                FileError("close artifact");
            }
            file_ = -1;
            RenameNoReplace(active_directory_path_, temporary_, active_name_);
            temporary_.clear();
            if (fsync(active_directory_) != 0) {
                FileError("commit staging");
            }
            manifest_.push_back({active_.id, active_.name, offset_});
            close(active_directory_);
            active_directory_ = -1;
            active_name_.clear();
        } else {
            throw std::runtime_error("ARTIFACT: unexpected message");
        }
    } catch (...) {
        Abort();
        throw;
    }
}
bool ArtifactReceiver::Validate(
    const ipc::Result& result, const ipc::Config& config, std::vector<std::string>* issues) const
{
    Require(file_ < 0, "ARTIFACT: unfinished artifact");
    bool complete = true;
    const auto checkCollection = [&](bool condition, const std::string& message) {
        if (!condition) {
            complete = false;
            if (issues != nullptr) {
                issues->push_back(message);
            }
        }
    };
    checkCollection(
        result.status == 0 && result.code == 0 && result.complete && result.errorCount == 0 && result.droppedCount == 0,
        "PROFILING: incomplete result");
    Require(
        result.sections == config.sections && result.manifest.size() == manifest_.size(),
        "ARTIFACT: manifest mismatch");
    std::set<std::string> expectedPmu;
    bool pipelineExpected = false;
    bool samplingExpected = false;
    for (const auto& section : config.sections) {
        if (section == "Pipeline") {
            pipelineExpected = true;
        } else if (section == "PmSampling") {
            samplingExpected = true;
        } else {
            expectedPmu.insert(section + ".csv");
        }
    }
    if (!expectedPmu.empty() || pipelineExpected) {
        expectedPmu.insert("summary.jsonl");
    }
    std::set<uint64_t> ids;
    std::map<std::string, std::set<std::string>> pmuGroups;
    std::map<std::string, std::set<std::string>> samplingGroups;
    bool hardwareFound = false;
    bool pipelineManifestFound = false;
    std::size_t pipelineFragments = 0;
    for (const auto& entry : result.manifest) {
        Require(ids.insert(entry.id).second, "ARTIFACT: duplicate manifest entry");
        const auto found = std::find_if(manifest_.begin(), manifest_.end(), [&](const auto& value) {
            return value.id == entry.id && value.name == entry.name && value.bytes == entry.bytes;
        });
        Require(found != manifest_.end(), "ARTIFACT: manifest length mismatch");
        const auto separator = entry.name.find_last_of('/');
        const std::string parent = separator == std::string::npos ? "" : entry.name.substr(0, separator);
        const std::string base = separator == std::string::npos ? entry.name : entry.name.substr(separator + 1);
        if (entry.name == "HardwareInfo.jsonl") {
            Require(!hardwareFound, "ARTIFACT: duplicate hardware information");
            hardwareFound = true;
        } else if (parent == ".biu-staging/process-uds") {
            Require(pipelineExpected, "ARTIFACT: unexpected Pipeline data");
            if (base == "manifest.json") {
                pipelineManifestFound = true;
            } else {
                ++pipelineFragments;
            }
        } else if (ipc::KindForName(entry.name) == ipc::DataKind::PmSampling) {
            Require(samplingExpected, "ARTIFACT: unexpected PmSampling artifact");
            Require(samplingGroups[parent].insert(base).second, "ARTIFACT: duplicate PmSampling artifact");
        } else {
            Require(expectedPmu.count(base) == 1, "ARTIFACT: unexpected PMU artifact");
            Require(pmuGroups[parent].insert(base).second, "ARTIFACT: duplicate PMU artifact");
        }
    }
    checkCollection(hardwareFound, "HardwareInfo.jsonl: required artifact missing");
    checkCollection(
        pipelineExpected == (pipelineManifestFound && pipelineFragments > 0),
        "ARTIFACT: required Pipeline artifact missing");
    if (samplingExpected) {
        checkCollection(!samplingGroups.empty(), "PmSampling collection: no result files received");
        const std::set<std::string> expected = {"summary.db", "objdump.txt", "symbolizer.json"};
        for (const auto& [parent, files] : samplingGroups) {
            if (!files.count("summary.db") && files.count("objdump.txt") &&
                HasOnlySimdVectorCode(directory_path_ / parent / "objdump.txt")) {
                checkCollection(
                    false,
                    (directory_path_ / parent / "objdump.txt").string() +
                        ": PmSampling requires SIMT instructions (VF_SIMT); only SIMD VF instructions were found. "
                        "PC/state samples are unavailable; other valid results are retained");
            }
            for (const auto& name : expected) {
                checkCollection(
                    files.count(name) != 0, (directory_path_ / parent / name).string() +
                                                ": PmSampling result missing; other valid results are retained");
            }
            for (const auto& name : files) {
                std::string error;
                const bool valid = ValidatePmSamplingFile(directory_path_ / parent / name, &error);
                checkCollection(valid, error);
            }
        }
    }
    if (!expectedPmu.empty()) {
        checkCollection(!pmuGroups.empty(), "ARTIFACT: required PMU artifact missing");
        for (const auto& [parent, files] : pmuGroups) {
            for (const auto& name : expectedPmu) {
                checkCollection(files.count(name) != 0, parent + "/" + name + ": required PMU artifact missing");
            }
        }
    } else {
        Require(pmuGroups.empty(), "ARTIFACT: unexpected PMU artifact");
    }
    return complete;
}
} // namespace npucompute::cli
