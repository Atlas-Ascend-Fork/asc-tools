/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "import/imported_profile_results.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "report/rep_decoder.h"

namespace npucompute::cli {
namespace {

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool FailErrno(const std::string& message, std::string* error)
{
    return Fail(message + ": " + std::error_code(errno, std::generic_category()).message(), error);
}

bool IsSafeName(const std::string& name)
{
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos;
}

bool RemoveSuffix(const std::string& name, const std::string& suffix, std::string* output)
{
    if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    *output = name.substr(0, name.size() - suffix.size());
    return true;
}

bool ResolveOutputName(const ImportedProfileEntry& entry, std::string* name, std::string* error)
{
    if (!IsSafeName(entry.name)) {
        return Fail("imported entry name is unsafe: " + entry.name, error);
    }
    if (entry.name == ".hardware_info.lock") {
        return Fail("HardwareInfo lock is not a collection result", error);
    }
    if (entry.type != NpuRepFileType::NpuRep) {
        *name = entry.name;
        return true;
    }
    if (!RemoveSuffix(entry.name, ".npu.rep", name) && !RemoveSuffix(entry.name, ".rep", name)) {
        return Fail("invalid nested report entry name: '" + entry.name + "'.", error);
    }
    if (!IsSafeName(*name)) {
        return Fail("imported child directory name is unsafe: " + *name, error);
    }
    return true;
}

bool IsKnownLeafType(NpuRepFileType type)
{
    switch (type) {
        case NpuRepFileType::Json:
        case NpuRepFileType::Jsonl:
        case NpuRepFileType::Csv:
        case NpuRepFileType::Sqlite:
        case NpuRepFileType::Txt:
            return true;
        case NpuRepFileType::NpuRep:
            return false;
    }
    return false;
}

bool ValidateImportedEntries(
    const std::vector<ImportedProfileEntry>& entries, const std::string& logical_path, std::string* error,
    std::size_t depth = 1U)
{
    if (depth > kNpuRepMaxDepth) {
        return Fail("maximum nested report depth exceeded: " + logical_path, error);
    }
    std::unordered_set<std::string> output_names;
    output_names.reserve(entries.size());
    for (const ImportedProfileEntry& entry : entries) {
        std::string output_name;
        if (!ResolveOutputName(entry, &output_name, error)) {
            return false;
        }
        if (!output_names.insert(output_name).second) {
            return Fail("imported entries conflict after unpacking: " + logical_path + "/" + output_name, error);
        }

        if (entry.type == NpuRepFileType::NpuRep) {
            if (!entry.payload.empty()) {
                return Fail("imported child rep contains leaf payload: " + entry.name, error);
            }
            if (!ValidateImportedEntries(entry.children, logical_path + "/" + output_name, error, depth + 1U)) {
                return false;
            }
            continue;
        }
        if (!IsKnownLeafType(entry.type)) {
            return Fail("imported entry has an unknown leaf type: " + entry.name, error);
        }
        if (!entry.children.empty()) {
            return Fail("imported leaf entry contains child entries: " + entry.name, error);
        }
    }
    return true;
}

bool ValidateOutputPaths(
    const std::vector<ImportedProfileEntry>& entries, const boost::filesystem::path& output_directory,
    std::string* error)
{
    for (const ImportedProfileEntry& entry : entries) {
        std::string output_name;
        if (!ResolveOutputName(entry, &output_name, error)) {
            return false;
        }
        const boost::filesystem::path output_path = output_directory / output_name;
        boost::system::error_code status_error;
        const boost::filesystem::file_status status = boost::filesystem::symlink_status(output_path, status_error);
        if (status_error == boost::system::errc::no_such_file_or_directory) {
            continue;
        }
        if (status_error) {
            return Fail(
                "inspect imported output path failed: " + output_path.string() + ": " + status_error.message(), error);
        }
        if (boost::filesystem::exists(status)) {
            return Fail("imported output path already exists: " + output_path.string(), error);
        }
    }
    return true;
}

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor) : descriptor_(descriptor) {}

    ~FileDescriptor()
    {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int Get() const { return descriptor_; }

    bool Close(const boost::filesystem::path& path, std::string* error)
    {
        if (descriptor_ < 0) {
            return true;
        }
        const int descriptor = descriptor_;
        descriptor_ = -1;
        if (::close(descriptor) != 0) {
            return FailErrno("close imported file failed: " + path.string(), error);
        }
        return true;
    }

private:
    int descriptor_ = -1;
};

class CreatedPathCleanup {
public:
    explicit CreatedPathCleanup(boost::filesystem::path path) : path_(std::move(path)) {}

    ~CreatedPathCleanup()
    {
        if (active_) {
            boost::system::error_code error;
            boost::filesystem::remove_all(path_, error);
        }
    }

    void Release() { active_ = false; }

private:
    boost::filesystem::path path_;
    bool active_ = true;
};

bool WriteAll(
    int descriptor, const std::vector<uint8_t>& payload, const boost::filesystem::path& path, std::string* error)
{
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const std::size_t request =
            std::min(payload.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = ::write(descriptor, payload.data() + offset, request);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FailErrno("write imported file failed: " + path.string(), error);
        }
        if (result == 0) {
            return Fail("write imported file returned zero: " + path.string(), error);
        }
        offset += static_cast<std::size_t>(result);
    }
    return true;
}

bool SyncDescriptor(int descriptor, const std::string& description, std::string* error)
{
    while (::fsync(descriptor) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return FailErrno("sync " + description + " failed", error);
    }
    return true;
}

bool WriteImportedFile(const boost::filesystem::path& path, const std::vector<uint8_t>& payload, std::string* error)
{
    FileDescriptor descriptor(
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
    if (descriptor.Get() < 0) {
        return FailErrno("create imported file failed: " + path.string(), error);
    }
    CreatedPathCleanup cleanup(path);
    if (!WriteAll(descriptor.Get(), payload, path, error) ||
        !SyncDescriptor(descriptor.Get(), "imported file: " + path.string(), error) || !descriptor.Close(path, error)) {
        return false;
    }
    cleanup.Release();
    return true;
}

bool SyncDirectory(const boost::filesystem::path& path, std::string* error)
{
    FileDescriptor descriptor(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (descriptor.Get() < 0) {
        return FailErrno("open imported directory for sync failed: " + path.string(), error);
    }
    return SyncDescriptor(descriptor.Get(), "imported directory: " + path.string(), error) &&
           descriptor.Close(path, error);
}

bool UnpackImportedEntries(
    const std::vector<ImportedProfileEntry>& entries, const boost::filesystem::path& output_directory,
    std::string* error)
{
    for (const ImportedProfileEntry& entry : entries) {
        std::string output_name;
        if (!ResolveOutputName(entry, &output_name, error)) {
            return false;
        }
        const boost::filesystem::path output_path = output_directory / output_name;
        if (entry.type != NpuRepFileType::NpuRep) {
            if (!WriteImportedFile(output_path, entry.payload, error)) {
                return false;
            }
            continue;
        }

        if (::mkdir(output_path.c_str(), S_IRWXU) != 0) {
            return FailErrno("create imported directory failed: " + output_path.string(), error);
        }
        CreatedPathCleanup cleanup(output_path);
        if (!UnpackImportedEntries(entry.children, output_path, error) || !SyncDirectory(output_path, error)) {
            return false;
        }
        cleanup.Release();
    }
    return SyncDirectory(output_directory, error);
}

bool InvalidInput(const std::string& file, const std::string& reason, std::string* error)
{
    return Fail("--import expects a report file, but '" + file + "' " + reason + ".", error);
}

bool ReadInputFile(const boost::filesystem::path& path, std::vector<uint8_t>* content, std::string* error)
{
    boost::system::error_code status_error;
    const boost::filesystem::file_status status = boost::filesystem::symlink_status(path, status_error);
    if (status_error == boost::system::errc::no_such_file_or_directory ||
        (!status_error && !boost::filesystem::exists(status))) {
        return InvalidInput(path.string(), "does not exist", error);
    }
    if (status_error) {
        return InvalidInput(path.string(), "cannot be inspected: " + status_error.message(), error);
    }
    if (boost::filesystem::is_directory(status)) {
        return InvalidInput(path.string(), "is a directory", error);
    }
    if (!boost::filesystem::is_regular_file(status)) {
        return InvalidInput(path.string(), "is not a supported file; symbolic links are not supported", error);
    }

    errno = 0;
    std::ifstream input(path.string(), std::ios::binary);
    if (!input.is_open()) {
        const int code = errno;
        const std::string reason = code == 0 ?
                                       "cannot be opened" :
                                       "cannot be opened: " + std::error_code(code, std::generic_category()).message();
        return InvalidInput(path.string(), reason, error);
    }
    try {
        content->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    } catch (const std::ios_base::failure& failure) {
        content->clear();
        return InvalidInput(path.string(), "cannot be read: " + std::string(failure.what()), error);
    }
    if (input.bad()) {
        content->clear();
        return InvalidInput(path.string(), "cannot be read", error);
    }
    return true;
}

bool InvalidReport(const std::string& file, const std::string& reason, std::string* error)
{
    return Fail("invalid npu-compute report '" + file + "': " + reason + ".", error);
}

bool DecodeImportedEntries(
    const std::vector<uint8_t>& encoded, const std::string& logical_path, std::vector<ImportedProfileEntry>* results,
    std::string* error, std::size_t depth = 1U)
{
    if (depth > kNpuRepMaxDepth) {
        return InvalidReport(logical_path, "maximum nested report depth exceeded", error);
    }
    DecodedRep decoded;
    std::string decode_error;
    RepDecodeError category;
    if (!DecodeRep(encoded, &decoded, &decode_error, &category)) {
        switch (category) {
            case RepDecodeError::Internal:
                return Fail(decode_error, error);
            case RepDecodeError::UnsupportedVersion:
                return InvalidReport(logical_path, "unsupported report version", error);
            case RepDecodeError::InvalidFormat:
                if (depth == 1U) {
                    return InvalidReport(
                        logical_path,
                        encoded.empty() ? "invalid file format: the file is empty" : "invalid file format", error);
                }
                [[fallthrough]];
            default:
                return InvalidReport(logical_path, "file is corrupted", error);
        }
    }

    std::vector<ImportedProfileEntry> imported;
    imported.reserve(decoded.entries.size());
    for (DecodedRepEntry& decoded_entry : decoded.entries) {
        ImportedProfileEntry entry;
        entry.name = std::move(decoded_entry.file_name);
        entry.type = decoded_entry.file_type;
        if (entry.type == NpuRepFileType::NpuRep) {
            if (!DecodeImportedEntries(decoded_entry.payload, logical_path, &entry.children, error, depth + 1U)) {
                return false;
            }
        } else {
            entry.payload = std::move(decoded_entry.payload);
        }
        imported.push_back(std::move(entry));
    }
    *results = std::move(imported);
    return true;
}

} // namespace

bool ReadImportedProfileResults(
    const boost::filesystem::path& input_path, std::vector<ImportedProfileEntry>* results, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (results == nullptr) {
        return Fail("imported profile results output is null", error);
    }
    results->clear();

    try {
        std::vector<uint8_t> encoded;
        if (!ReadInputFile(input_path, &encoded, error) ||
            !DecodeImportedEntries(encoded, input_path.string(), results, error)) {
            results->clear();
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        results->clear();
        return Fail("allocate imported profile results failed", error);
    }
}

bool UnpackImportedProfileResults(
    const std::vector<ImportedProfileEntry>& results, const boost::filesystem::path& output_directory,
    std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }

    try {
        boost::system::error_code status_error;
        const boost::filesystem::file_status status = boost::filesystem::symlink_status(output_directory, status_error);
        if (status_error) {
            return Fail(
                "inspect import output directory failed: " + output_directory.string() + ": " + status_error.message(),
                error);
        }
        if (!boost::filesystem::is_directory(status)) {
            return Fail("import output path is not a directory: " + output_directory.string(), error);
        }
        if (!ValidateImportedEntries(results, output_directory.string(), error)) {
            return false;
        }
        if (!ValidateOutputPaths(results, output_directory, error)) {
            return false;
        }
        return UnpackImportedEntries(results, output_directory, error);
    } catch (const std::bad_alloc&) {
        return Fail("allocate imported output state failed", error);
    }
}

} // namespace npucompute::cli
