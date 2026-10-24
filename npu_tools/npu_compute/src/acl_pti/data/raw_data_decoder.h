/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_RAW_DATA_DECODER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_RAW_DATA_DECODER_H

#include "data_types.h"

namespace aclpti::data {
inline constexpr std::size_t TASK_LOG_BYTES = 32;
inline constexpr std::size_t PMU_RECORD_BYTES = 128;

// Caller must provide at least four readable bytes; alignment is unrestricted.
uint32_t ReadLittleEndianWord(const uint8_t* bytes);

// Without slot configuration, decode metadata only and leave the PMU event map empty.
DecodeResult DecodeRawRecord(const std::byte* data, std::size_t size, uint64_t recordIndex);

// Accepts exactly one 32-byte task log or 128-byte PMU record; never retains data.
DecodeResult DecodeRawRecord(
    const std::byte* data, std::size_t size, uint64_t recordIndex, const PmuSlots& pmuEventIds);
} // namespace aclpti::data

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_RAW_DATA_DECODER_H
