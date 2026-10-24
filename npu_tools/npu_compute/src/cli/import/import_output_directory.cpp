/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "import/import_output_directory.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace npucompute::cli {
namespace {

constexpr char NPU_REP_SUFFIX[] = ".npu-rep";
constexpr char NESTED_NPU_REP_SUFFIX[] = ".npu.rep";
constexpr char REP_SUFFIX[] = ".rep";
constexpr std::size_t MAXIMUM_NAME_ATTEMPTS = 128U;
constexpr std::size_t MKDTEMP_RANDOM_LENGTH = 6U;

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

bool ReadStatus(const boost::filesystem::path& path, boost::filesystem::file_status* status, std::string* error)
{
    boost::system::error_code statusError;
    *status = boost::filesystem::symlink_status(path, statusError);
    if (statusError == boost::system::errc::no_such_file_or_directory) {
        *status = boost::filesystem::file_status(boost::filesystem::file_not_found);
        return true;
    }
    if (statusError) {
        return Fail("inspect import output path failed: " + path.string() + ": " + statusError.message(), error);
    }
    return true;
}

boost::filesystem::path ResolvePath(
    const boost::filesystem::path& path, const boost::filesystem::path& currentDirectory)
{
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return (currentDirectory / path).lexically_normal();
}

bool RemoveSuffix(std::string* name)
{
    const char* const suffixes[] = {NPU_REP_SUFFIX, NESTED_NPU_REP_SUFFIX, REP_SUFFIX};
    for (const char* suffix : suffixes) {
        const std::size_t suffixLength = std::strlen(suffix);
        if (name->size() >= suffixLength && name->compare(name->size() - suffixLength, suffixLength, suffix) == 0) {
            name->resize(name->size() - suffixLength);
            return !name->empty();
        }
    }
    return false;
}

bool ValidateInputName(const boost::filesystem::path& inputRep, std::string* error)
{
    std::string name = inputRep.filename().string();
    if (!RemoveSuffix(&name)) {
        return Fail(
            "--import expects a report file, but '" + inputRep.string() + "' has an unsupported report file name.",
            error);
    }
    return true;
}

bool ResolveOutputRoot(
    const boost::filesystem::path& inputRep, const std::optional<std::string>& exportPath,
    boost::filesystem::path* outputRoot, std::string* error)
{
    if (!ValidateInputName(inputRep, error)) {
        return false;
    }

    boost::system::error_code currentPathError;
    const boost::filesystem::path currentDirectory = boost::filesystem::current_path(currentPathError);
    if (currentPathError) {
        return Fail("get current directory failed: " + currentPathError.message(), error);
    }

    if (exportPath.has_value()) {
        if (exportPath->empty()) {
            return Fail("import export path is empty", error);
        }
        *outputRoot = ResolvePath(*exportPath, currentDirectory);
    } else {
        *outputRoot = currentDirectory;
    }

    boost::filesystem::file_status outputRootStatus;
    if (!ReadStatus(*outputRoot, &outputRootStatus, error)) {
        return false;
    }
    if (!boost::filesystem::exists(outputRootStatus)) {
        return Fail(
            "--export directory '" + outputRoot->string() + "' does not exist. Create it before importing.", error);
    }
    if (!boost::filesystem::is_directory(outputRootStatus)) {
        return Fail("--export requires an existing directory when importing: '" + outputRoot->string() + "'.", error);
    }
    return true;
}

bool CreateTemporaryDirectory(
    const boost::filesystem::path& outputRoot, boost::filesystem::path* temporaryPath,
    boost::filesystem::path* finalPath, std::string* error)
{
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    if (milliseconds.count() < 0) {
        return Fail("import output directory timestamp is before the Unix epoch", error);
    }

    const std::string namePrefix =
        "npu-compute-import-" + std::to_string(milliseconds.count()) + "-" + std::to_string(::getpid()) + "-";
    for (std::size_t attempt = 0; attempt < MAXIMUM_NAME_ATTEMPTS; ++attempt) {
        std::string pathTemplate = (outputRoot / ("." + namePrefix + "tmp-XXXXXX")).string();
        pathTemplate.push_back('\0');
        char* created = ::mkdtemp(pathTemplate.data());
        if (created == nullptr) {
            return FailErrno("create import temporary directory failed in '" + outputRoot.string() + "'", error);
        }

        const boost::filesystem::path createdPath = created;
        const std::string temporaryName = createdPath.filename().string();
        const std::string randomSuffix = temporaryName.substr(temporaryName.size() - MKDTEMP_RANDOM_LENGTH);
        const boost::filesystem::path candidateFinalPath = outputRoot / (namePrefix + randomSuffix);
        boost::filesystem::file_status finalStatus;
        if (!ReadStatus(candidateFinalPath, &finalStatus, error)) {
            boost::system::error_code removeError;
            boost::filesystem::remove(createdPath, removeError);
            return false;
        }
        if (!boost::filesystem::exists(finalStatus)) {
            *temporaryPath = createdPath;
            *finalPath = candidateFinalPath;
            return true;
        }

        boost::system::error_code removeError;
        if (!boost::filesystem::remove(createdPath, removeError)) {
            return Fail(
                "remove colliding import temporary directory failed: " + createdPath.string() + ": " +
                    removeError.message(),
                error);
        }
    }
    return Fail("unable to generate a unique import output directory after 128 attempts", error);
}

int RenameNoReplace(const char* source, const char* target)
{
#if defined(SYS_renameat2)
    constexpr long RENAME_AT2 = SYS_renameat2;
#elif defined(__NR_renameat2)
    constexpr long RENAME_AT2 = __NR_renameat2;
#elif defined(__linux__) && defined(__LP64__) && !defined(__ILP32__) && defined(__x86_64__)
    // Linux x86_64 LP64 syscall ABI; do not use this number for x32.
    constexpr long RENAME_AT2 = 316;
#elif defined(__linux__) && defined(__LP64__) && !defined(__ILP32__) && defined(__aarch64__)
    // Linux AArch64 LP64 uses the asm-generic syscall ABI.
    constexpr long RENAME_AT2 = 276;
#else
    constexpr long RENAME_AT2 = -1;
#endif
#if defined(RENAME_NOREPLACE)
    constexpr unsigned int NO_REPLACE = RENAME_NOREPLACE;
#else
    constexpr unsigned int NO_REPLACE = 1U;
#endif
    if (RENAME_AT2 < 0) {
        errno = ENOSYS;
        return -1;
    }
    // Preserve the kernel error; a non-atomic fallback could overwrite the target.
    return static_cast<int>(::syscall(RENAME_AT2, AT_FDCWD, source, AT_FDCWD, target, NO_REPLACE));
}

bool SyncDirectory(const boost::filesystem::path& directory, std::string* error)
{
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return FailErrno("open import output parent for sync failed", error);
    }
    while (::fsync(descriptor) != 0) {
        if (errno == EINTR) {
            continue;
        }
        const int syncError = errno;
        ::close(descriptor);
        errno = syncError;
        return FailErrno("sync import output parent failed", error);
    }
    if (::close(descriptor) != 0) {
        return FailErrno("close import output parent failed", error);
    }
    return true;
}

} // namespace

ImportOutputDirectory::~ImportOutputDirectory() { CleanupTemporaryDirectory(); }

bool ImportOutputDirectory::Create(
    const boost::filesystem::path& inputRep, const std::optional<std::string>& exportPath,
    ImportOutputDirectory* directory, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (directory == nullptr) {
        return Fail("import output directory is null", error);
    }
    directory->CleanupTemporaryDirectory();
    directory->finalPath_.clear();

    boost::filesystem::path outputRoot;
    if (!ResolveOutputRoot(inputRep, exportPath, &outputRoot, error)) {
        return false;
    }
    if (!CreateTemporaryDirectory(outputRoot, &directory->temporaryPath_, &directory->finalPath_, error)) {
        directory->finalPath_.clear();
        return false;
    }
    return true;
}

const boost::filesystem::path& ImportOutputDirectory::TemporaryPath() const { return temporaryPath_; }

const boost::filesystem::path& ImportOutputDirectory::FinalPath() const { return finalPath_; }

bool ImportOutputDirectory::Publish(std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (temporaryPath_.empty() || finalPath_.empty()) {
        return Fail("import output directory is not initialized", error);
    }

    const int result = RenameNoReplace(temporaryPath_.c_str(), finalPath_.c_str());
    if (result != 0) {
        return FailErrno("publish import output directory failed: '" + finalPath_.string() + "'", error);
    }
    temporaryPath_.clear();
    if (!SyncDirectory(finalPath_.parent_path(), error)) {
        boost::system::error_code removeError;
        boost::filesystem::remove_all(finalPath_, removeError);
        return false;
    }
    return true;
}

void ImportOutputDirectory::CleanupTemporaryDirectory() noexcept
{
    if (!temporaryPath_.empty()) {
        boost::system::error_code error;
        boost::filesystem::remove_all(temporaryPath_, error);
        temporaryPath_.clear();
    }
}

} // namespace npucompute::cli
