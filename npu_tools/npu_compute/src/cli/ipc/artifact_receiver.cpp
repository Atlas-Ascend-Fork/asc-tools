/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "common/section_names.h"
#include "communication/artifact_schema.h"
#include "artifact_receiver.h"
#include "file_io.h"
#include "report/pm_sampling_file_validator.h"
#include <algorithm>
#include <boost/filesystem.hpp>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <map>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace npucompute::cli {
namespace {

constexpr std::string_view kSummaryFileName = "summary.jsonl";

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
using ArtifactGroups = std::map<std::string, std::set<std::string>>;
struct ManifestGroups {
    ArtifactGroups pmuGroups;
    ArtifactGroups samplingGroups;
    bool hardwareFound = false;
    bool pipelineManifestFound = false;
    bool communicationManifestFound = false;
    std::size_t pipelineFragments = 0;
};

bool HasManifestEntry(const std::vector<ipc::ManifestEntry>& manifest, const ipc::ManifestEntry& entry)
{
    return std::any_of(manifest.begin(), manifest.end(), [&entry](const auto& value) {
        return value.id == entry.id && value.name == entry.name && value.bytes == entry.bytes;
    });
}

ManifestGroups CollectManifestGroups(
    const ipc::Result& result, const std::vector<ipc::ManifestEntry>& manifest,
    const std::set<std::string>& expectedPmu, bool pipelineExpected, bool samplingExpected, bool sourceExpected)
{
    std::set<uint64_t> ids;
    ManifestGroups groups;
    for (const auto& entry : result.manifest) {
        Require(ids.insert(entry.id).second, "ARTIFACT: duplicate manifest entry");
        Require(HasManifestEntry(manifest, entry), "ARTIFACT: manifest length mismatch");
        const auto separator = entry.name.find_last_of('/');
        const std::string parent = separator == std::string::npos ? "" : entry.name.substr(0, separator);
        const std::string base = separator == std::string::npos ? entry.name : entry.name.substr(separator + 1);
        if (entry.name == "HardwareInfo.jsonl") {
            Require(!groups.hardwareFound, "ARTIFACT: duplicate hardware information");
            groups.hardwareFound = true;
        } else if (parent == communication::kProcessDirectory) {
            Require(IsCommunicationMode(result.sections), "unexpected Communication data");
            if (base == "manifest.json")
                groups.communicationManifestFound = true;
        } else if (parent == ".biu-staging/process-uds") {
            Require(pipelineExpected, "ARTIFACT: unexpected Pipeline data");
            if (base == ipc::kManifestFileName) {
                groups.pipelineManifestFound = true;
            } else {
                ++groups.pipelineFragments;
            }
        } else if (ipc::KindForName(entry.name) == ipc::DataKind::PmSampling) {
            Require(
                samplingExpected || (sourceExpected && base != "symbolizer.json"),
                "ARTIFACT: unexpected instruction report artifact");
            Require(groups.samplingGroups[parent].insert(base).second, "ARTIFACT: duplicate PmSampling artifact");
        } else {
            Require(
                base == ipc::kOccupancyArtifactName || expectedPmu.count(base) == 1,
                "ARTIFACT: unexpected PMU artifact");
            Require(groups.pmuGroups[parent].insert(base).second, "ARTIFACT: duplicate PMU artifact");
        }
    }
    return groups;
}

struct CollectionValidation {
    std::vector<std::string>* issues = nullptr;
    bool complete = true;

    void Check(bool condition, const std::string& message)
    {
        if (!condition) {
            complete = false;
            if (issues != nullptr) {
                issues->push_back(message);
            }
        }
    }
};

void ValidateSamplingGroup(
    const boost::filesystem::path& directory, const std::string& parent, const std::set<std::string>& files,
    const ArtifactGroups& pmuGroups, CollectionValidation& validation)
{
    const auto pmu = pmuGroups.find(parent);
    validation.Check(
        pmu != pmuGroups.end() && pmu->second.count("summary.jsonl") != 0,
        (directory / parent / "summary.jsonl").string() + ": required summary artifact missing");
    for (const auto& name : files) {
        std::string error;
        const bool valid = ValidatePmSamplingFile(directory / parent / name, &error);
        validation.Check(valid, error);
    }
}

} // namespace
ArtifactReceiver::ArtifactReceiver(const std::string& directory)
    : directory_path_(boost::filesystem::absolute(directory))
{
    directory_ = asctools::security::OpenDirectory(directory);
    if (directory_ < 0) {
        FileError("open staging");
    }
}
ArtifactReceiver::~ArtifactReceiver()
{
    CleanupArtifactFile();
    if (directory_ >= 0) {
        close(directory_);
    }
}
void ArtifactReceiver::CleanupArtifactFile() noexcept
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
void ArtifactReceiver::OpenArtifactDirectory()
{
    const auto separator = active_.name.find_last_of('/');
    active_name_ = separator == std::string::npos ? active_.name : active_.name.substr(separator + 1);
    active_directory_path_ = directory_path_;
    active_directory_ = dup(directory_);
    if (active_directory_ < 0) {
        FileError("duplicate staging descriptor");
    }
    if (separator != std::string::npos) {
        const std::string parent = active_.name.substr(0, separator);
        size_t start = 0;
        while (start < parent.size()) {
            const auto end = parent.find('/', start);
            const auto component = parent.substr(start, end == std::string::npos ? std::string::npos : end - start);
            active_directory_path_ /= component;
            const int next = OpenOrCreateDirectoryAt(active_directory_, component);
            close(active_directory_);
            active_directory_ = next;
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
}
void ArtifactReceiver::BeginArtifact(const ipc::Frame& frame)
{
    Require(active_.id == 0, "ARTIFACT: interleaved begin");
    auto begin = ipc::DecodeArtifactBegin(frame.payload);
    Require(
        begin.id != 0 && begin.kind == ipc::KindForName(begin.name) &&
            begin.artifactType == ipc::ArtifactTypeForName(begin.name),
        "ARTIFACT: invalid name or kind");
    for (const auto& entry : manifest_) {
        Require(entry.id != begin.id && entry.name != begin.name, "ARTIFACT: duplicate artifact");
    }
    active_ = std::move(begin);
    offset_ = 0;
    OpenArtifactDirectory();
    temporary_ = "." + active_name_ + "." + std::to_string(active_.id) + ".tmp";
    file_ = openat(
        active_directory_, temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
        (S_IRUSR | S_IWUSR));
    if (file_ < 0) {
        temporary_.clear();
        FileError("create artifact");
    }
}
void ArtifactReceiver::ReceiveChunk(const ipc::Frame& frame)
{
    Require(frame.type == ipc::TypeForKind(active_.kind), "ARTIFACT: data kind mismatch");
    auto chunk = ipc::DecodeArtifactChunk(frame.payload);
    Require(chunk.id == active_.id && chunk.offset == offset_, "ARTIFACT: invalid offset or id");
    offset_ += chunk.data.size();
    if (file_ < 0) {
        return;
    }
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
}
void ArtifactReceiver::EndArtifact(const ipc::Frame& frame)
{
    const auto end = ipc::DecodeArtifactEnd(frame.payload);
    Require(end.id == active_.id && end.bytes == offset_, "ARTIFACT: invalid end length");
    if (file_ >= 0) {
        CommitArtifactFile();
    }
    CompleteArtifact();
}
void ArtifactReceiver::CommitArtifactFile()
{
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
        const int syncError = errno;
        if (active_name_ == ipc::kOccupancyArtifactName) {
            unlinkat(active_directory_, active_name_.c_str(), 0);
        }
        errno = syncError;
        FileError("commit staging");
    }
}
void ArtifactReceiver::CompleteArtifact()
{
    // Match the sender's manifest even when an optional file was discarded locally.
    manifest_.push_back({active_.id, active_.name, offset_});
    CleanupArtifactFile();
    active_.id = 0;
}
void ArtifactReceiver::Receive(const ipc::Frame& frame)
{
    try {
        if (frame.type == ipc::Type::DataBegin) {
            BeginArtifact(frame);
            return;
        }
        Require(active_.id != 0, "ARTIFACT: data without begin");
        switch (frame.type) {
            case ipc::Type::Pmu:
            case ipc::Type::Pipeline:
            case ipc::Type::Hardware:
            case ipc::Type::Communication:
            case ipc::Type::PmSampling:
            case ipc::Type::Application:
                ReceiveChunk(frame);
                break;
            case ipc::Type::DataEnd:
                EndArtifact(frame);
                break;
            default:
                throw std::runtime_error("ARTIFACT: unexpected message");
        }
    } catch (const ipc::Exception& error) {
        const bool skipFile = error.Detail().domain == "FILESYSTEM" && active_name_ == ipc::kOccupancyArtifactName;
        CleanupArtifactFile();
        if (!skipFile) {
            active_.id = 0;
            throw;
        }
        std::fprintf(stderr, "[WARNING] npu-compute: skipped %s: %s\n", active_.name.c_str(), error.what());
        if (frame.type == ipc::Type::DataEnd) {
            CompleteArtifact();
        }
    } catch (...) {
        CleanupArtifactFile();
        active_.id = 0;
        throw;
    }
}
namespace {
std::set<std::string> ExpectedPmuFiles(
    const ipc::Config& config, bool& pipelineExpected, bool& samplingExpected, bool& sourceExpected)
{
    std::set<std::string> expectedPmu;
    bool rooflineExpected = false;
    for (const auto& section : config.sections) {
        if (section == "Pipeline") {
            pipelineExpected = true;
        } else if (section == "PmSampling") {
            samplingExpected = true;
        } else if (section == "SourceCounter") {
            sourceExpected = true;
        } else if (section == "Roofline") {
            rooflineExpected = true;
        } else if (section == "occupancy") {
            expectedPmu.insert(ipc::kSimtOccupancyFileName);
        } else if (section != "Communication") {
            expectedPmu.insert(section + ".csv");
        }
    }
    if (!expectedPmu.empty() || pipelineExpected || samplingExpected || sourceExpected || rooflineExpected) {
        expectedPmu.insert("summary.jsonl");
    }
    return expectedPmu;
}
void WarnMissingSamplingFiles(const boost::filesystem::path& directory, const ManifestGroups& groups)
{
    // Missing sampling files can also mean that a SIMD kernel produced no PC data.
    for (const auto& group : groups.pmuGroups) {
        const auto sampling = groups.samplingGroups.find(group.first);
        for (const auto* name : {ipc::kSummaryDbFileName, ipc::kObjdumpFileName, ipc::kSymbolizerFileName}) {
            if (sampling == groups.samplingGroups.end() || sampling->second.count(name) == 0) {
                std::fprintf(
                    stderr, "[WARNING] npu-compute: PmSampling result file was not produced: %s\n",
                    (directory / group.first / name).c_str());
            }
        }
    }
}

void ValidatePmuGroups(
    const ManifestGroups& groups, const std::set<std::string>& expectedPmu, bool warpOptional,
    CollectionValidation& validation)
{
    if (!expectedPmu.empty()) {
        validation.Check(!groups.pmuGroups.empty(), "ARTIFACT: required PMU artifact missing");
        for (const auto& [parent, files] : groups.pmuGroups) {
            for (const auto& name : expectedPmu) {
                if (files.count(name) != 0) {
                    continue;
                }
                if (warpOptional && name == ipc::kSimtOccupancyFileName) {
                    const auto path = parent.empty() ? name : parent + "/" + name;
                    std::fprintf(
                        stderr, "[WARNING] npu-compute: occupancy result file was not produced: %s\n", path.c_str());
                } else {
                    validation.Check(false, parent + "/" + name + ": required PMU artifact missing");
                }
            }
        }
    } else {
        Require(groups.pmuGroups.empty(), "ARTIFACT: unexpected PMU artifact");
    }
}

void ValidateSourceCounterGroups(
    const boost::filesystem::path& directory, const ManifestGroups& groups, CollectionValidation& validation)
{
    for (const auto& group : groups.pmuGroups) {
        const auto& parent = group.first;
        const auto instructionFiles = groups.samplingGroups.find(parent);
        const std::set<std::string> emptyFiles;
        const auto& files = instructionFiles == groups.samplingGroups.end() ? emptyFiles : instructionFiles->second;
        validation.Check(
            files.count(ipc::kObjdumpFileName) != 0,
            (directory / parent / ipc::kObjdumpFileName).string() + ": required SourceCounter disassembly missing");
        validation.Check(
            files.count(ipc::kSummaryDbFileName) != 0,
            (directory / parent / ipc::kSummaryDbFileName).string() + ": required SourceCounter database missing");
    }
}

void ValidateApplicationManifest(
    const std::vector<ipc::ManifestEntry>& resultManifest, const std::vector<ipc::ManifestEntry>& manifest,
    CollectionValidation& validation)
{
    std::set<std::string> names;
    for (const auto& entry : resultManifest) {
        Require(HasManifestEntry(manifest, entry), "ARTIFACT: manifest length mismatch");
        Require(
            ipc::KindForName(entry.name) == ipc::DataKind::Application ||
                ipc::KindForName(entry.name) == ipc::DataKind::Communication || entry.name == "HardwareInfo.jsonl" ||
                (entry.name.rfind("device", 0) == 0 && entry.name.size() >= kSummaryFileName.size() &&
                 entry.name.compare(
                     entry.name.size() - kSummaryFileName.size(), kSummaryFileName.size(), kSummaryFileName.data()) ==
                     0),
            "ARTIFACT: unexpected application artifact");
        Require(names.insert(entry.name).second, "ARTIFACT: duplicate application artifact");
    }
    validation.Check(names.count("HardwareInfo.jsonl") != 0, "HardwareInfo.jsonl: required artifact missing");
    validation.Check(
        names.count(std::string(communication::kArtifactPrefix) + ipc::kManifestFileName) != 0,
        "Communication manifest missing");
}

} // namespace

bool ArtifactReceiver::Validate(
    const ipc::Result& result, const ipc::Config& config, std::vector<std::string>* issues) const
{
    Require(active_.id == 0, "ARTIFACT: unfinished artifact");
    CollectionValidation validation{issues};
    validation.Check(
        result.status == 0 && result.code == 0 && result.complete && result.errorCount == 0 && result.droppedCount == 0,
        "PROFILING: incomplete result");
    Require(
        result.sections == config.sections && result.manifest.size() == manifest_.size(),
        "ARTIFACT: manifest mismatch");
    if (config.replayMode == "application") {
        ValidateApplicationManifest(result.manifest, manifest_, validation);
        return validation.complete;
    }
    bool pipelineExpected = false;
    bool samplingExpected = false;
    bool sourceExpected = false;
    const auto expectedPmu = ExpectedPmuFiles(config, pipelineExpected, samplingExpected, sourceExpected);
    const auto groups =
        CollectManifestGroups(result, manifest_, expectedPmu, pipelineExpected, samplingExpected, sourceExpected);
    if (IsCommunicationMode(config.sections)) {
        validation.Check(groups.communicationManifestFound, "Communication manifest missing");
    }
    if (!IsCommunicationMode(config.sections)) {
        validation.Check(groups.hardwareFound, "HardwareInfo.jsonl: required artifact missing");
    }
    validation.Check(
        pipelineExpected == (groups.pipelineManifestFound && groups.pipelineFragments > 0),
        "ARTIFACT: required Pipeline artifact missing");
    if (samplingExpected) {
        WarnMissingSamplingFiles(directory_path_, groups);
    }
    if (sourceExpected) {
        ValidateSourceCounterGroups(directory_path_, groups, validation);
    }
    for (const auto& [parent, files] : groups.samplingGroups) {
        ValidateSamplingGroup(directory_path_, parent, files, groups.pmuGroups, validation);
    }
    const bool warpOptional = config.sections.size() > 1;
    ValidatePmuGroups(groups, expectedPmu, warpOptional, validation);
    return validation.complete;
}
} // namespace npucompute::cli
