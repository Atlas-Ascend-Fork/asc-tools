/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_SOURCE_COUNTER_SQLITE_WRITER_H
#define NPU_COMPUTE_SOURCE_COUNTER_SQLITE_WRITER_H

#include "aclpti/aclpti_data.h"
#include <string>

namespace npucompute {
// Appends the table and units in one transaction. Creates the table even when there are no rows.
bool WriteSourceCounterSqlite(
    const std::string& path, const aclptiSourceCounterData& data, uint64_t& writtenRows, std::string& error);
} // namespace npucompute

#endif
