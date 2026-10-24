/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_INITCHECK_REPORT_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_INITCHECK_REPORT_H

#include "acl_san/aclsan_cbdata_device.h"
#include "diagnostic/report_message.h"

namespace npucheck {

// Shared context and access fields; callers add their allocation and check results.
NpuCheckInitcheckReport BuildInitcheckMemoryReportBase(
    const AclsanDeviceMemoryAccessData& data, uint64_t allocationBytes, uint32_t allocationDeviceId);

} // namespace npucheck

#endif
