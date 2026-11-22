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
    uint64_t addr = 0;      // args[0]: base address
    int64_t offset = 0;     // args[1]: signed element or byte offset, per instruction family
    uint64_t post = 0;      // args[2]: post-index flag
    uint64_t sysVaBase = 0; // args[3]: captured on the executing core
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
    uint64_t addr = 0;         // args[0]: base address
    int64_t offset = 0;        // args[1]: signed element or byte offset, per instruction family
    uint64_t post = 0;         // args[2]: post-index flag
    uint64_t sysVaBase = 0;    // args[3]: captured on the executing core
    uint32_t atomicConfig = 0; // Captured ST_ATOMIC_CFG; zero is a valid configuration.
};

// Scalar ATOM (427-430). No offset or post addressing.
// Bisheng supplies the operation type through the -1 ctrl.bin selector.
struct ScalarAtomParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0;
    uint64_t dstValue = 0;            // args[0] holds the captured destination register value.
    uint64_t srcAddr = 0;             // args[1] holds the address of the atomic read-modify-write.
    uint64_t srcOperand = 0;          // args[2] holds the first source operand.
    uint64_t srcOperandCas = 0;       // args[3] holds the second source operand, used only by CAS.
    uint64_t sysVaBase = 0;           // 用于地址空间判断的 SYS_VA_BASE 窗口位。
    uint64_t atomicType = UINT64_MAX; // args[4] encodes 0=CAS, 1=EXCH, 2=ADD, 3=MIN, 4=MAX.
};

// Scalar RED (431-435) has one source operand and no destination register.
struct ScalarRedParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0;
    uint64_t srcAddr = 0;             // args[0] holds the address of the atomic read-modify-write.
    uint64_t srcOperand = 0;          // args[1] holds the source operand.
    uint64_t sysVaBase = 0;           // 用于地址空间判断的 SYS_VA_BASE 窗口位。
    uint64_t atomicType = UINT64_MAX; // args[2] holds the operation type, using the same encoding as ATOM.
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_SCALAR_H
