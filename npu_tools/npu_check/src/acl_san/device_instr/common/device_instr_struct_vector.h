/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef ACLSAN_DEVICE_INSTR_STRUCT_VECTOR_H
#define ACLSAN_DEVICE_INSTR_STRUCT_VECTOR_H

#include <array>
#include <cstdint>

namespace aclsan {

struct PushPbParamField {
    std::array<uint64_t, 4> values{};
};

struct VfParamField {
    uint64_t entryPc = 0;
    uint64_t instructionCount = 0;
};

} // namespace aclsan

#endif // ACLSAN_DEVICE_INSTR_STRUCT_VECTOR_H
