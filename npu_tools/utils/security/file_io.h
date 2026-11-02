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

namespace asctools::security {
enum class PathKind {
    ExistingFile,
    ExistingDirectory,
    FileOrNew,
};

int OpenDirectory(const std::string& path);
bool ValidatePath(const std::string& path, PathKind kind);
bool ReadTextFile(const std::string& path, std::string& content, std::size_t maxBytes = 0);
bool WriteFile(const std::string& path, std::string_view content);
} // namespace asctools::security

#endif
