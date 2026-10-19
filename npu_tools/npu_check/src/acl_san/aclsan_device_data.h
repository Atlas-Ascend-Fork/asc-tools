/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_DEVICE_DATA_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_DEVICE_DATA_H

#include "acl_san/aclsan_api.h"
#include "device_instr/decoder.h"
#include "aclsan_memory_cbdata.h"
#include "aclsan_trace_buffer.h"

#include <optional>
#include <variant>
#include <vector>

namespace aclsan {

// 一条搬运指令可能会返回多个cbdata，x条src, y条dst
using DeviceMemoryAccessDataList = std::vector<AclsanDeviceMemoryAccessData>;
using DeviceCallbackData = std::variant<DeviceMemoryAccessDataList, AclsanDeviceSyncData, AclsanDeviceBarrierData>;

// 根据 ParsedTraceRecord 和 paramField 提取最终返回的 cbdata，配上对应的日志
std::optional<DeviceCallbackData> TranslateDecodedTraceToCallbackData(
    const ParsedTraceRecord& parsed, const aclsan::DecodedInstruction& decoded,
    const MemoryRegisterState& registerState = {}, const std::vector<InternalMemoryRange>* internalInputs = nullptr,
    const std::vector<InternalMemoryRange>* readOnlyBinaryRanges = nullptr) noexcept;

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_DEVICE_DATA_H
