/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_vf.h"
#include "mock.h"
#include <cassert>
int main()
{
    const unsigned char object[] = {0x7f, 'E', 'L', 'F'};
    auto handle = aclsan::NpubitUbHandle::Create(object, sizeof(object));
    assert(handle);
    aclsan::ParsedTraceRecord vf{};
    vf.physicalCoreSlotCount = 108;
    vf.phyCoreId = 18;
    vf.launchId = 1;
    vf.instrExecId = 1;
    vf.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    hibit_test::ranges = {{0x200, 0, 4, NPUBIT_UB_ACCESS_VECTOR_READ}};
    AclsanDeviceVfData completion{};
    assert(handle->Query(vf, {}, {}, &completion).empty());
    assert(completion.status == ACLSAN_DEVICE_VF_UNSUPPORTED);
    assert(hibit_test::frees == 1);
}
