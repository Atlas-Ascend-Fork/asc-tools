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

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = boost::filesystem;
namespace security = asctools::security;

class SecurityFileIo : public testing::Test {
protected:
    void SetUp() override
    {
        const std::string pattern = (fs::temp_directory_path() / "security-file-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        ASSERT_NE(::mkdtemp(buffer.data()), nullptr);
        root = buffer.data();
    }
    void TearDown() override
    {
        boost::system::error_code error;
        if (!root.empty()) {
            fs::remove_all(root, error);
        }
    }
    fs::path root;
};

TEST_F(SecurityFileIo, CreatesAndTruncatesFileLikeOutputStream)
{
    const auto path = (root / "ctrl.bin").string();
    std::string expected(70000, 'x');
    expected[1] = '\0';
    ASSERT_TRUE(security::WriteFile(path, expected));
    struct stat status {};
    ASSERT_EQ(::stat(path.c_str(), &status), 0);
    const auto reference = (root / "reference").string();
    {
        std::ofstream stream(reference, std::ios::binary);
        ASSERT_TRUE(stream);
    }
    struct stat referenceStatus {};
    ASSERT_EQ(::stat(reference.c_str(), &referenceStatus), 0);
    EXPECT_EQ(status.st_mode & 0777, referenceStatus.st_mode & 0777);
    std::string actual;
    ASSERT_TRUE(security::ReadTextFile(path, actual, expected.size()));
    EXPECT_EQ(actual, expected);
    ASSERT_TRUE(security::ReadTextFile(path, actual));
    EXPECT_EQ(actual, expected);
    ASSERT_EQ(::chmod(path.c_str(), 0640), 0);
    ASSERT_TRUE(security::WriteFile(path, "replacement"));
    ASSERT_TRUE(security::ReadTextFile(path, actual, expected.size()));
    EXPECT_EQ(actual, "replacement");
    ASSERT_EQ(::stat(path.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0640);
    ASSERT_TRUE(security::WriteFile(path, ""));
    ASSERT_TRUE(security::ReadTextFile(path, actual, 0));
    EXPECT_TRUE(actual.empty());
}

TEST_F(SecurityFileIo, RejectsOversizedReadWithoutChangingOutput)
{
    const auto path = (root / "online").string();
    ASSERT_TRUE(security::WriteFile(path, "0-15\n"));
    std::string content = "unchanged";
    EXPECT_FALSE(security::ReadTextFile(path, content, 4));
    EXPECT_EQ(errno, EFBIG);
    EXPECT_EQ(content, "unchanged");
    ASSERT_TRUE(security::ReadTextFile(path, content, 5));
    EXPECT_EQ(content, "0-15\n");
    const auto empty = (root / "empty").string();
    ASSERT_TRUE(security::WriteFile(empty, ""));
    ASSERT_TRUE(security::ReadTextFile(empty, content, 0));
    EXPECT_TRUE(content.empty());
}

TEST_F(SecurityFileIo, RejectsFinalSymlinksAndSpecialFiles)
{
    const auto file = root / "target";
    ASSERT_TRUE(security::WriteFile(file.string(), "original"));
    fs::create_symlink(file, root / "link");
    std::string content = "unchanged";
    EXPECT_FALSE(security::ReadTextFile((root / "link").string(), content, 64));
    EXPECT_FALSE(security::WriteFile((root / "link").string(), "replacement"));
    ASSERT_TRUE(security::ReadTextFile(file.string(), content, 64));
    EXPECT_EQ(content, "original");
    fs::create_directory_symlink(root, root / "dir-link");
    EXPECT_EQ(security::OpenDirectory((root / "dir-link").string()), -1);
    EXPECT_EQ(security::OpenDirectory((root / "dir-link").string() + "/"), -1);
    EXPECT_EQ(security::OpenDirectory(file.string()), -1);
    EXPECT_FALSE(security::ReadTextFile(root.string(), content, 64));
    const auto fifo = (root / "fifo").string();
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    EXPECT_FALSE(security::ReadTextFile(fifo, content, 64));
    EXPECT_FALSE(security::WriteFile(fifo, "x"));
    EXPECT_FALSE(security::WriteFile(root.string(), "x"));
}

TEST_F(SecurityFileIo, AllowsParentSymlinksAndReturnsOwnedDirectoryDescriptor)
{
    fs::create_directory(root / "real");
    fs::create_directory_symlink(root / "real", root / "alias");
    ASSERT_TRUE(security::WriteFile((root / "alias" / "value").string(), "42"));
    std::string content;
    ASSERT_TRUE(security::ReadTextFile((root / "alias" / "value").string(), content, 2));
    EXPECT_EQ(content, "42");
    const int descriptor = security::OpenDirectory((root / "real").string() + "/");
    ASSERT_GE(descriptor, 0);
    EXPECT_NE(::fcntl(descriptor, F_GETFD) & FD_CLOEXEC, 0);
    EXPECT_EQ(::fsync(descriptor), 0);
    EXPECT_EQ(::close(descriptor), 0);
}

TEST_F(SecurityFileIo, RejectsInvalidPathsAndMissingParents)
{
    const auto file = (root / "value").string();
    const std::string embeddedNull = file + std::string("\0suffix", 7);
    std::string content = "unchanged";
    EXPECT_FALSE(security::WriteFile(embeddedNull, "x"));
    EXPECT_FALSE(fs::exists(file));
    EXPECT_FALSE(security::WriteFile(file + "/", "x"));
    EXPECT_FALSE(fs::exists(file));
    EXPECT_FALSE(security::ReadTextFile(embeddedNull, content, 10));
    EXPECT_EQ(security::OpenDirectory(embeddedNull), -1);
    EXPECT_EQ(security::OpenDirectory(""), -1);
    EXPECT_FALSE(security::WriteFile("", "x"));
    EXPECT_FALSE(security::ReadTextFile("", content, 10));
    EXPECT_FALSE(security::WriteFile((root / "absent" / "file").string(), "x"));
    EXPECT_EQ(content, "unchanged");
}

TEST_F(SecurityFileIo, ValidatesExistingAndNewFilePaths)
{
    const auto file = (root / "value").string();
    EXPECT_FALSE(security::ValidatePath(file, security::PathKind::ExistingFile));
    EXPECT_TRUE(security::ValidatePath(file, security::PathKind::FileOrNew));
    ASSERT_TRUE(security::WriteFile(file, "value"));
    EXPECT_TRUE(security::ValidatePath(file, security::PathKind::ExistingFile));
    EXPECT_FALSE(security::ValidatePath(root.string(), security::PathKind::ExistingFile));
    EXPECT_TRUE(security::ValidatePath(root.string(), security::PathKind::ExistingDirectory));
}

TEST_F(SecurityFileIo, ReadsVirtualFileWithoutRelyingOnStatSize)
{
    // Linux procfs, like sysfs, reports size zero for generated regular files.
    std::string content;
    ASSERT_TRUE(security::ReadTextFile("/proc/self/status", content, 64U * 1024U));
    EXPECT_NE(content.find("Name:"), std::string::npos);
}

TEST_F(SecurityFileIo, NormalizesNewFileThroughExistingParent)
{
    fs::create_directory(root / "real");
    fs::create_directory_symlink(root / "real", root / "alias");
    const auto path = (root / "alias" / "." / "value").string();
    std::string normalized;
    ASSERT_TRUE(security::NormalizePath(path, security::PathKind::FileOrNew, normalized));
    EXPECT_EQ(normalized, (fs::canonical(root / "real") / "value").string());
    EXPECT_FALSE(fs::exists(normalized));
    ASSERT_TRUE(security::WriteFile(normalized, "value"));
    ASSERT_TRUE(security::NormalizePath(path, security::PathKind::ExistingFile, normalized));
    EXPECT_EQ(normalized, (fs::canonical(root / "real") / "value").string());
}

TEST_F(SecurityFileIo, NormalizationRejectsInvalidAndSymbolicLinkTargets)
{
    ASSERT_TRUE(security::WriteFile((root / "target").string(), "original"));
    fs::create_symlink(root / "target", root / "link");
    std::string normalized = "unchanged";
    for (const auto& path :
         {std::string{}, (root / "link").string(), (root / "missing" / "value").string(),
          (root / "value").string() + "/", (root / "value").string() + std::string("\0suffix", 7)}) {
        EXPECT_FALSE(security::NormalizePath(path, security::PathKind::FileOrNew, normalized)) << path;
        EXPECT_EQ(normalized, "unchanged");
    }
    EXPECT_FALSE(security::NormalizePath(root.string(), security::PathKind::ExistingFile, normalized));
    EXPECT_TRUE(security::NormalizePath(root.string() + "/", security::PathKind::ExistingDirectory, normalized));
    EXPECT_EQ(normalized, fs::canonical(root).string());
}

TEST_F(SecurityFileIo, ExclusiveWritePreservesExistingFileAndCreationMode)
{
    const auto path = (root / "manifest").string();
    ASSERT_TRUE(security::WriteExclusiveFile(path, "original"));
    EXPECT_FALSE(security::WriteExclusiveFile(path, "replacement"));
    EXPECT_EQ(errno, EEXIST);
    std::string content;
    ASSERT_TRUE(security::ReadTextFile(path, content));
    EXPECT_EQ(content, "original");
    struct stat status {};
    ASSERT_EQ(::stat(path.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    fs::create_symlink(root / "missing", root / "link");
    EXPECT_FALSE(security::WriteExclusiveFile((root / "link").string(), "x"));
    EXPECT_FALSE(fs::exists(root / "missing"));
}

TEST_F(SecurityFileIo, OpensCacheLockWithRestrictedPermissionsAndCloseOnExec)
{
    const auto path = (root / "cache.lock").string();
    const int descriptor = security::OpenFile(path, O_CREAT | O_RDWR, 0600);
    ASSERT_GE(descriptor, 0);
    struct stat status {};
    ASSERT_EQ(::fstat(descriptor, &status), 0);
    EXPECT_TRUE(S_ISREG(status.st_mode));
    EXPECT_EQ(status.st_mode & 0777, 0600);
    EXPECT_NE(::fcntl(descriptor, F_GETFD) & FD_CLOEXEC, 0);
    EXPECT_EQ(::close(descriptor), 0);
    fs::create_symlink(root / "cache.lock", root / "link");
    EXPECT_EQ(security::OpenFile((root / "link").string(), O_CREAT | O_RDWR, 0600), -1);
}
} // namespace
