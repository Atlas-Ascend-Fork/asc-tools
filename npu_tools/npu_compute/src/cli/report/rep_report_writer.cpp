/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/rep_report_writer.h"

#include "report/rep_format.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace npucompute::cli {
namespace {

constexpr std::size_t MAXIMUM_TEMPORARY_ATTEMPTS = 128U;
std::atomic<uint64_t> g_temporarySequence{0U};

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool FailErrno(const std::string& message, int error_number, std::string* error)
{
    return Fail(message + ": " + std::error_code(error_number, std::generic_category()).message(), error);
}

uint16_t ReadLe16(const uint8_t* data) { return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1]) << 8U; }

uint32_t ReadLe32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8U |
           static_cast<uint32_t>(data[2]) << 16U | static_cast<uint32_t>(data[3]) << 24U;
}

uint64_t ReadLe64(const uint8_t* data)
{
    uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<uint64_t>(data[index]) << (index * 8U);
    }
    return value;
}

bool HasMagic(const uint8_t* data) { return std::equal(NPU_REP_MAGIC.begin(), NPU_REP_MAGIC.end(), data); }

bool ValidateRepBytes(const std::vector<uint8_t>& encoded, std::string* error)
{
    if (encoded.size() < NPU_REP_MAGIC.size() || !HasMagic(encoded.data())) {
        return Fail("invalid rep header", error);
    }
    if (encoded.size() < NPU_REP_HEADER_FIELDS_END) {
        return Fail("truncated rep header fields", error);
    }
    const uint16_t head_length = ReadLe16(encoded.data() + 14U);
    const uint32_t file_count = ReadLe32(encoded.data() + 16U);
    const uint32_t file_info_length = ReadLe32(encoded.data() + 20U);
    const uint64_t rep_length = ReadLe64(encoded.data() + 28U);
    if (head_length < NPU_REP_HEADER_FIELDS_END || head_length > encoded.size() ||
        file_info_length != NPU_REP_FILE_INFO_SIZE || rep_length != encoded.size()) {
        return Fail("invalid rep header lengths", error);
    }

    const uint64_t table_length = static_cast<uint64_t>(file_count) * file_info_length;
    const uint64_t payload_start = head_length + table_length;
    if (payload_start > encoded.size()) {
        return Fail("rep file info table exceeds input", error);
    }

    uint64_t expected_offset = payload_start;
    for (uint32_t index = 0; index < file_count; ++index) {
        const uint64_t info_offset = head_length + static_cast<uint64_t>(index) * file_info_length;
        const uint8_t* info = encoded.data() + info_offset;
        if (!HasMagic(info)) {
            return Fail("invalid rep file info magic", error);
        }
        const char* name = reinterpret_cast<const char*>(info + 8U);
        const char* name_end = static_cast<const char*>(std::memchr(name, '\0', NPU_REP_FILE_NAME_SIZE));
        if (name_end == nullptr || name_end == name) {
            return Fail("invalid rep file info name", error);
        }
        const uint64_t file_length = ReadLe64(info + 144U);
        const uint64_t file_offset = ReadLe64(info + 152U);
        if (file_offset != expected_offset || file_offset > encoded.size() ||
            file_length > encoded.size() - file_offset) {
            return Fail("invalid rep file payload range", error);
        }
        expected_offset += file_length;
    }
    if (expected_offset != encoded.size()) {
        return Fail("rep contains unreferenced payload bytes", error);
    }
    return true;
}

ssize_t SystemWrite(int descriptor, const void* data, std::size_t size, void*)
{
    return ::write(descriptor, data, size);
}

int SystemSync(int descriptor, void*) { return ::fsync(descriptor); }

int SystemClose(int descriptor, void*) { return ::close(descriptor); }

int SystemRename(const char* source, const char* target, void*)
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

class FileDescriptor {
public:
    FileDescriptor(int value, const ReportFileOperations* operations) : value_(value), operations_(operations) {}

    ~FileDescriptor()
    {
        if (value_ >= 0) {
            operations_->close(value_, operations_->context);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int Get() const { return value_; }

    bool Close(const std::string& description, std::string* error)
    {
        if (value_ < 0) {
            return true;
        }
        const int descriptor = value_;
        value_ = -1;
        if (operations_->close(descriptor, operations_->context) != 0) {
            return FailErrno("close " + description + " failed", errno, error);
        }
        return true;
    }

private:
    int value_ = -1;
    const ReportFileOperations* operations_ = nullptr;
};

class TemporaryFileCleanup {
public:
    explicit TemporaryFileCleanup(boost::filesystem::path path) : path_(std::move(path)) {}

    ~TemporaryFileCleanup()
    {
        if (active_) {
            ::unlink(path_.c_str());
        }
    }

    void Release() { active_ = false; }

private:
    boost::filesystem::path path_;
    bool active_ = true;
};

bool ReadStatus(const boost::filesystem::path& path, boost::filesystem::file_status* status, std::string* error)
{
    boost::system::error_code status_error;
    *status = boost::filesystem::symlink_status(path, status_error);
    if (status_error == boost::system::errc::no_such_file_or_directory) {
        *status = boost::filesystem::file_status(boost::filesystem::file_not_found);
        return true;
    }
    if (status_error) {
        return Fail("inspect report path failed: " + path.string() + ": " + status_error.message(), error);
    }
    return true;
}

bool WriteAll(
    int descriptor, const std::vector<uint8_t>& encoded, const ReportFileOperations& operations, std::string* error)
{
    std::size_t offset = 0;
    while (offset < encoded.size()) {
        const std::size_t request =
            std::min(encoded.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = operations.write(descriptor, encoded.data() + offset, request, operations.context);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FailErrno("write temporary rep failed", errno, error);
        }
        if (result == 0) {
            return Fail("write temporary rep returned zero", error);
        }
        offset += static_cast<std::size_t>(result);
    }
    return true;
}

bool SyncDescriptor(
    int descriptor, const std::string& description, const ReportFileOperations& operations, std::string* error)
{
    while (operations.sync(descriptor, operations.context) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return FailErrno("sync " + description + " failed", errno, error);
    }
    return true;
}

bool ReadFile(const boost::filesystem::path& path, std::vector<uint8_t>* content, std::string* error)
{
    std::ifstream input(path.string(), std::ios::binary);
    if (!input.is_open()) {
        return Fail("open temporary rep for verification failed: " + path.string(), error);
    }
    content->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        content->clear();
        return Fail("read temporary rep for verification failed: " + path.string(), error);
    }
    return true;
}

bool CreateTemporaryFile(
    const boost::filesystem::path& directory, const std::string& target_name, boost::filesystem::path* temporary_path,
    int* descriptor, std::string* error)
{
    for (std::size_t attempt = 0; attempt < MAXIMUM_TEMPORARY_ATTEMPTS; ++attempt) {
        const uint64_t sequence = g_temporarySequence.fetch_add(1U, std::memory_order_relaxed);
        *temporary_path =
            directory / ("." + target_name + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(sequence));
        const int value =
            ::open(temporary_path->c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (value >= 0) {
            *descriptor = value;
            return true;
        }
        if (errno != EEXIST) {
            return FailErrno("create temporary rep failed: " + temporary_path->string(), errno, error);
        }
    }
    return Fail("unable to create a unique temporary rep after 128 attempts", error);
}

} // namespace

bool PublishRepReport(const std::vector<uint8_t>& encoded, const ReportTarget& target, std::string* error)
{
    ReportFileOperations operations;
    operations.write = &SystemWrite;
    operations.sync = &SystemSync;
    operations.close = &SystemClose;
    operations.rename = &SystemRename;
    return PublishRepReportWithOperations(encoded, target, operations, error);
}

bool PublishRepReportWithOperations(
    const std::vector<uint8_t>& encoded, const ReportTarget& target, const ReportFileOperations& operations,
    std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (operations.write == nullptr || operations.sync == nullptr || operations.close == nullptr ||
        operations.rename == nullptr) {
        return Fail("report file operations are incomplete", error);
    }

    try {
        if (!ValidateRepBytes(encoded, error)) {
            return false;
        }
        if (target.path.empty() || target.path.filename().empty()) {
            return Fail("report target path is empty", error);
        }
        boost::filesystem::path directory = target.path.parent_path();
        if (directory.empty()) {
            directory = ".";
        }
        boost::filesystem::file_status directory_status;
        if (!ReadStatus(directory, &directory_status, error) || !boost::filesystem::is_directory(directory_status)) {
            if (error != nullptr && error->empty()) {
                *error = "report target parent is not a directory: " + directory.string();
            }
            return false;
        }

        boost::filesystem::file_status target_status;
        if (!ReadStatus(target.path, &target_status, error)) {
            return false;
        }
        if (boost::filesystem::exists(target_status)) {
            if (!boost::filesystem::is_regular_file(target_status)) {
                return Fail("report target exists but is not a regular file: " + target.path.string(), error);
            }
            return Fail(
                "output report '" + target.path.string() + "' already exists. Choose a different file name.", error);
        }

        boost::filesystem::path temporary_path;
        int temporary_value = -1;
        if (!CreateTemporaryFile(
                directory, target.path.filename().string(), &temporary_path, &temporary_value, error)) {
            return false;
        }
        FileDescriptor temporary_descriptor(temporary_value, &operations);
        TemporaryFileCleanup temporary_cleanup(temporary_path);
        if (!WriteAll(temporary_descriptor.Get(), encoded, operations, error) ||
            !SyncDescriptor(
                temporary_descriptor.Get(), "temporary report '" + temporary_path.string() + "'", operations, error) ||
            !temporary_descriptor.Close("temporary report '" + temporary_path.string() + "'", error)) {
            return false;
        }

        std::vector<uint8_t> readback;
        if (!ReadFile(temporary_path, &readback, error) || readback != encoded) {
            if (error != nullptr && error->empty()) {
                *error = "temporary rep verification does not match input";
            }
            return false;
        }
        if (!ValidateRepBytes(readback, error)) {
            return false;
        }

        const int directory_value = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory_value < 0) {
            return FailErrno("open report output directory failed: " + directory.string(), errno, error);
        }
        FileDescriptor directory_descriptor(directory_value, &operations);
        if (operations.rename(temporary_path.c_str(), target.path.c_str(), operations.context) != 0) {
            return FailErrno("publish rep report failed: " + target.path.string(), errno, error);
        }
        temporary_cleanup.Release();
        if (!SyncDescriptor(
                directory_descriptor.Get(), "report output directory '" + directory.string() + "'", operations,
                error) ||
            !directory_descriptor.Close("report output directory '" + directory.string() + "'", error)) {
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return Fail("allocate report publishing buffer failed", error);
    }
}

} // namespace npucompute::cli
