/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_DEVICE_DATA_LOG_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_DEVICE_DATA_LOG_H

#include "aclsan_device_data.h"

namespace aclsan {

void LogRawRecord(const ParsedTraceRecord& parsed) noexcept;
void LogParamField(const aclsan::DeviceInstructionParamField& params) noexcept;
void LogCallbackData(const AclsanDeviceRegisterStateData& cbData) noexcept;
void LogCallbackData(const DeviceCallbackData& callbackData) noexcept;

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_DEVICE_DATA_LOG_H
