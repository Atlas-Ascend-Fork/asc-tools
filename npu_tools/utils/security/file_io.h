/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef ASC_TOOLS_UTILS_SECURITY_FILE_IO_H
#define ASC_TOOLS_UTILS_SECURITY_FILE_IO_H

#include <cstddef>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace asctools::security {
enum class PathKind {
    ExistingFile,
    ExistingDirectory,
    FileOrNew,
};

int OpenDirectory(const std::string& path);
int OpenFile(const std::string& path, int flags, mode_t mode = 0666);
// 将已存在的父目录解析为实际绝对路径（解析其中的符号链接、. 和 ..），并校验目标类型。
// ExistingFile 要求目标为普通文件；ExistingDirectory 要求目标为目录；
// FileOrNew 允许目标文件不存在，但父目录必须存在，已存在的目标必须为普通文件。
// 拒绝空路径、含 NUL 的路径及末级符号链接；文件路径不能以 / 结尾。
// 成功时返回 true 并写入 normalized；返回 false 时保持 normalized 不变。
// 仅检查当前路径，不创建或打开目标文件；实际读写仍需使用安全文件操作接口。
bool NormalizePath(const std::string& path, PathKind kind, std::string& normalized);
bool ValidatePath(const std::string& path, PathKind kind);
bool ReadTextFile(const std::string& path, std::string& content, std::size_t maxBytes = 0);
bool WriteFile(const std::string& path, std::string_view content);
bool WriteFile(const std::string& path, std::string_view content, mode_t mode);
bool WriteExclusiveFile(const std::string& path, std::string_view content, mode_t mode = 0600);
} // namespace asctools::security

#endif
