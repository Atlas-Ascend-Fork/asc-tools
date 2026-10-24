/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_LAUNCH_DISK_OUTPUT_DIRECTORY_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_LAUNCH_DISK_OUTPUT_DIRECTORY_H

#include "report/report_name.h"
#include <optional>
#include <string>

namespace npucompute::cli {

bool ResolveDiskParentDirectory(
    const std::optional<std::string>& export_path, boost::filesystem::path* parent, std::string* error);

class DiskOutputDirectory {
public:
    DiskOutputDirectory() = default;
    ~DiskOutputDirectory();
    DiskOutputDirectory(const DiskOutputDirectory&) = delete;
    DiskOutputDirectory& operator=(const DiskOutputDirectory&) = delete;

    static bool Create(const boost::filesystem::path& parent, DiskOutputDirectory* result, std::string* error);
    static bool CreateWithSources(
        const boost::filesystem::path& parent, const ReportNameSources& sources, DiskOutputDirectory* result,
        std::string* error);

    const std::string& Path() const noexcept { return path_; }
    void Retain() noexcept { retained_ = true; }
    bool Remove(std::string* error);

private:
    std::string path_;
    bool retained_ = false;
};

} // namespace npucompute::cli
#endif
