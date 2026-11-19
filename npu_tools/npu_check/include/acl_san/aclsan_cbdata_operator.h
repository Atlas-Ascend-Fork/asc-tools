/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_OPERATOR_H
#define NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_OPERATOR_H

#include "acl_san/aclsan_cbdata_common.h"

#include <stdint.h>

// 算子执行完毕边界事件的标志位。
typedef enum AclsanOperatorExecutionFlags {
    ACLSAN_OPERATOR_EXECUTION_FLAG_NONE = 0,
    // 设备侧 trace 采集边界不完整（插桩/元数据不可用、同步超时后设备 reset 等），
    // 分析侧据此把本次算子的分析标记为不完整。
    ACLSAN_OPERATOR_EXECUTION_FLAG_INCOMPLETE = 1u << 0u,
    // 本次算子的设备侧 trace 记录存在丢失（dropped 计数非零）。
    ACLSAN_OPERATOR_EXECUTION_FLAG_DROPPED = 1u << 1u,
} AclsanOperatorExecutionFlags;

// 一个算子（kernel launch）的设备侧 trace 已采集完成，属于该 launch 的 cbdata
// 已全部派发。launchId 与该 launch 的 AclsanLaunchData.launchId 相同，分析侧
// 据此确定哪些 cbdata 属于同一算子并可以开始分析，支持多算子并发场景。
typedef struct AclsanOperatorExecutionData {
    AclsanCallbackCommonData common; // Callback metadata and result.
    uint64_t launchId;               // 与该 launch 的 AclsanLaunchData.launchId 关联。
    void* function;                  // aclrtFuncHandle；可能为 NULL。
    void* stream;                    // 本次 launch 提交到的 aclrtStream。
    uint32_t deviceId;               // 目标 device ID。
    uint32_t numBlocks;              // launch API 传入的原始 block 数量。
    uint32_t flags;                  // AclsanOperatorExecutionFlags。
    uint32_t reserved;               // Reserved for future use.
} AclsanOperatorExecutionData;

#endif // NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_OPERATOR_H
