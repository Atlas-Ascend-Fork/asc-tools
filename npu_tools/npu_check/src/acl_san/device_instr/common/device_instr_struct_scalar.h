/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_SCALAR_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_SCALAR_H

#include <cstdint>

namespace aclsan {

// A5 ST/STI/STP/LD/LDP scalar GM/UB access. REG offsets are element counts; IMM and pair offsets are bytes.
// post == 1 uses the captured base for this access without adding the offset.
struct ScalarGmParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0;
    uint64_t addr = 0;           // args[0]: base address
    int64_t offset = 0;          // args[1]: signed element or byte offset, per instruction family
    uint64_t post = 0;           // args[2]: post-index flag
    uint64_t sysVaBase = 0;      // args[3]: captured on the executing core
    uint64_t addressContext = 0; // args[4]: protocol marker; zero means missing
};

// ST_DEV/LD_DEV.b64/b32/b16/b8 always access GM and have no post-index flag.
struct ScalarDevParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0;
    uint64_t addr = 0;  // args[0]: base address
    int64_t offset = 0; // args[1]: signed byte offset, independent of dataBits
};

struct ScalarPreloadParamField {
    uint32_t instrId = 0;
    uint64_t addr = 0;
    int64_t offset = 0;
};

// Scalar atomic store. offset keeps the raw encoding: an element count for
// ST_ATOMIC.b32/b16/b8 (56-58) and a byte count for STI_ATOMIC.b32/b16/b8 (59-61).
// post == 1 uses the captured base for this access without adding the offset.
struct ScalarAtomicParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0;
    uint64_t addr = 0;           // args[0]: base address
    int64_t offset = 0;          // args[1]: signed element or byte offset, per instruction family
    uint64_t post = 0;           // args[2]: post-index flag
    uint64_t sysVaBase = 0;      // args[3]: captured on the executing core
    uint64_t addressContext = 0; // args[4]: protocol marker; zero means missing
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_SCALAR_H
