/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/* Minimal file I/O helpers for the standalone ctrl.bin writer.
 * Provides only the write-path validation and permission hooks used by ctrlbin_writer.cpp.
 */
#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_CTRLBIN_CTRLBIN_FILE_IO_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_CTRLBIN_CTRLBIN_FILE_IO_H

#include <string>
#include <sys/types.h>

const mode_t SAVE_DATA_FILE_AUTHORITY = 0640;

inline bool CheckWriteFilePathValid(std::string& path) { return !path.empty(); }

inline bool Chmod(const std::string& path, mode_t /*mode*/)
{
    (void)path;
    return true;
}

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_CTRLBIN_CTRLBIN_FILE_IO_H
