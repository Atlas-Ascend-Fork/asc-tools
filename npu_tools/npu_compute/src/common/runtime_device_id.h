/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_COMPUTE_COMMON_RUNTIME_DEVICE_ID_H
#define NPU_COMPUTE_COMMON_RUNTIME_DEVICE_ID_H

#include <acl/acl.h>
#include <acl/acl_rt.h>
#include <cstdint>
#include <cstdlib>

namespace npucompute {

inline aclError ResolveLogicDeviceId(int32_t userDeviceId, int32_t& logicDeviceId)
{
    if (std::getenv("ASCEND_RT_VISIBLE_DEVICES") == nullptr) {
        logicDeviceId = userDeviceId;
        return ACL_SUCCESS;
    }
    using Convert = aclError (*)(int32_t, int32_t*);
    static const auto convert = []() -> Convert {
        aclrtApiFunc original = nullptr;
        if (aclrtApiInjectionGetFunc("aclrtGetLogicDevIdByUserDevId", &original, nullptr) != ACL_SUCCESS) {
            return nullptr;
        }
        return reinterpret_cast<Convert>(reinterpret_cast<void*>(original));
    }();
    return convert == nullptr ? ACL_ERROR_RT_FAILURE : convert(userDeviceId, &logicDeviceId);
}

} // namespace npucompute

#endif // NPU_COMPUTE_COMMON_RUNTIME_DEVICE_ID_H
