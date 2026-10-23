/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_SQLITE_WRITER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_SQLITE_WRITER_H

#include "pm_sampling_types.h"
#include <string>

namespace npucompute {
// Writes PmSampling in one transaction and closes the database before returning.
// The caller supplies a private staging path and owns cleanup/packaging; on failure
// a file may remain and must not be packaged. No CSV conversion is performed.
bool WritePmSamplingSqlite(const std::string& path, const PmSamplingResult& result, std::string& error);
} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_SQLITE_WRITER_H
