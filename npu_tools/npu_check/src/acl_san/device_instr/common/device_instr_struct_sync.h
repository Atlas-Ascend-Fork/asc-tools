/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_SYNC_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_SYNC_H

#include <cstdint>

namespace aclsan {
struct PipeBarrierParamField {
    uint32_t instrId = 0;
    uint32_t pipe = 0;
};

// =====================================
// ========   SET/WAIT FLAG    =========
// =====================================

// - SET_FLAG.<src_pipe>.<dst_pipe> / SET_FLAGI.<src_pipe>.<dst_pipe>
// - SET_FLAG_V.<dst_pipe> / SET_FLAGI_V.<dst_pipe>，srcPipe 固定为 PIPE_V
// - WAIT_FLAG.<src_pipe>.<dst_pipe> / WAIT_FLAGI.<src_pipe>.<dst_pipe>
// - WAIT_FLAG_V.<src_pipe> / WAIT_FLAGI_V.<src_pipe>，dstPipe 固定为 PIPE_V
struct FlagParamField {
    uint32_t instrId = 0;
    uint32_t srcPipe = 0;
    uint32_t dstPipe = 0;
    uint64_t eventId = 0;
};

// =====================================
// ========     GET/RLS BUF    =========
// =====================================

// - GET_BUF.<pipe> / GET_BUFI.<pipe>
// - RLS_BUF.<pipe> / RLS_BUFI.<pipe>
// - GET_BUF_V / GET_BUFI_V，pipe 固定为 PIPE_V
// - RLS_BUF_V / RLS_BUFI_V，pipe 固定为 PIPE_V
struct SyncBufParamField {
    uint32_t instrId = 0;
    uint32_t pipe = 0;
    uint64_t bufId = 0;
    uint8_t mode = 0;
};

// =====================================
// ========    暂时用不到       =========
// =====================================

// 核间同步指令，对应CrossCoreWaitFlag
// WAIT_FLAG_DEV.<pipe> / WAIT_FLAG_DEVI.<pipe>
// WAIT_FLAG_DEV_V / WAIT_FLAG_DEVI_V，pipe 固定为 PIPE_V
struct DeviceFlagParamField {
    uint32_t instrId = 0;
    uint32_t pipe = 0;
    uint64_t flagId = 0;
};

// FFTS cross-core SET record. The mode and flag ID are packed in the raw
// probe config by the device-side DBI probe.
struct FftsSyncParamField {
    uint32_t instrId = 0;
    uint32_t dstPipe = 0;
    uint32_t mode = 0;
    uint64_t flagId = 0;
};

// - HSET_FLAG.<src_pipe>.<dst_pipe> / HSET_FLAGI.<src_pipe>.<dst_pipe>
// - HWAIT_FLAG.<src_pipe>.<dst_pipe> / HWAIT_FLAGI.<src_pipe>.<dst_pipe>
struct HardwareFlagParamField {
    uint32_t instrId = 0;
    uint32_t srcPipe = 0;
    uint32_t dstPipe = 0;
    uint64_t eventId = 0;
    uint64_t memory = 0;
    bool isVirtual = false;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_SYNC_H
