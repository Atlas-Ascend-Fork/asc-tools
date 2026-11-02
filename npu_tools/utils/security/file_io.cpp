/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "file_io.h"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace asctools::security {
namespace {
constexpr int INVALID_FILE_DESCRIPTOR = -1;

class FileDescriptor {
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor()
    {
        const int savedError = errno;
        if (value_ >= 0) {
            ::close(value_);
        }
        errno = savedError;
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int Get() const { return value_; }
    int Release()
    {
        const int value = value_;
        value_ = INVALID_FILE_DESCRIPTOR;
        return value;
    }
    bool Close() { return ::close(Release()) == 0; }

private:
    int value_;
};

int OpenChecked(const std::string& path, int flags, bool directory)
{
    if (path.empty() || path.find('\0') != std::string::npos) {
        errno = EINVAL;
        return INVALID_FILE_DESCRIPTOR;
    }
    if (!directory && path.back() == '/') {
        errno = EISDIR;
        return INVALID_FILE_DESCRIPTOR;
    }

    const auto last = path.find_last_not_of('/');
    const boost::filesystem::path input(last == std::string::npos ? "/" : path.substr(0, last + 1));
    boost::system::error_code error;
    const auto parent = boost::filesystem::canonical(input.has_parent_path() ? input.parent_path() : ".", error);
    if (error) {
        errno = error.value();
        return INVALID_FILE_DESCRIPTOR;
    }
    const FileDescriptor parentFd(::open(parent.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (parentFd.Get() < 0) {
        return INVALID_FILE_DESCRIPTOR;
    }
    FileDescriptor descriptor(::openat(
        parentFd.Get(), input.filename().c_str(), flags | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH));
    if (descriptor.Get() < 0) {
        return INVALID_FILE_DESCRIPTOR;
    }
    struct stat status {};
    if (::fstat(descriptor.Get(), &status) != 0) {
        return INVALID_FILE_DESCRIPTOR;
    }
    if (directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)) {
        errno = directory ? ENOTDIR : EINVAL;
        return INVALID_FILE_DESCRIPTOR;
    }
    return descriptor.Release();
}

bool IsValidPath(const std::string& path, bool directory)
{
    FileDescriptor descriptor(OpenChecked(path, O_PATH, directory));
    return descriptor.Get() >= 0;
}

bool IsValidParent(const std::string& path)
{
    const auto last = path.find_last_of('/');
    const std::string parent = last == std::string::npos ? "." : path.substr(0, last == 0 ? 1 : last);
    return IsValidPath(parent, true);
}

bool ReadAll(int descriptor, std::size_t maxBytes, std::string& result)
{
    constexpr std::size_t READ_CHUNK_SIZE = 4096U;
    char buffer[READ_CHUNK_SIZE];
    for (;;) {
        const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return true;
        }
        const auto size = static_cast<std::size_t>(count);
        if (maxBytes != 0 && (result.size() > maxBytes || size > maxBytes - result.size())) {
            errno = EFBIG;
            return false;
        }
        result.append(buffer, size);
    }
}

bool WriteAll(int descriptor, std::string_view content)
{
    constexpr std::size_t WRITE_CHUNK_SIZE = 64U * 1024U;
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t count =
            ::write(descriptor, content.data() + offset, std::min(content.size() - offset, WRITE_CHUNK_SIZE));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = EIO;
            }
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}
} // namespace

int OpenDirectory(const std::string& path) { return OpenChecked(path, O_RDONLY | O_DIRECTORY, true); }

bool ValidatePath(const std::string& path, PathKind kind)
{
    if (path.empty() || path.find('\0') != std::string::npos) {
        errno = EINVAL;
        return false;
    }
    switch (kind) {
        case PathKind::ExistingFile:
            return IsValidPath(path, false);
        case PathKind::ExistingDirectory:
            return IsValidPath(path, true);
        case PathKind::FileOrNew:
            if (IsValidPath(path, false)) {
                return true;
            }
            if (errno != ENOENT) {
                return false;
            }
            return IsValidParent(path);
    }
    errno = EINVAL;
    return false;
}

bool ReadTextFile(const std::string& path, std::string& content, std::size_t maxBytes)
{
    FileDescriptor descriptor(OpenChecked(path, O_RDONLY | O_NONBLOCK, false));
    if (descriptor.Get() < 0) {
        return false;
    }
    std::string result;
    if (!ReadAll(descriptor.Get(), maxBytes, result) || !descriptor.Close()) {
        return false;
    }
    content.swap(result);
    return true;
}

bool WriteFile(const std::string& path, std::string_view content)
{
    FileDescriptor descriptor(OpenChecked(path, O_WRONLY | O_CREAT | O_NONBLOCK, false));
    if (descriptor.Get() < 0) {
        return false;
    }
    if (::ftruncate(descriptor.Get(), 0) != 0) {
        return false;
    }
    return WriteAll(descriptor.Get(), content) && descriptor.Close();
}
} // namespace asctools::security
