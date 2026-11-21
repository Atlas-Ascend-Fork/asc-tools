/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_UB_LAYOUT_BUILDER_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_UB_LAYOUT_BUILDER_H

#include "acl_san/aclsan_ub_layout.h"
#include "device_instr/soc_version.h"

namespace aclsan {

inline constexpr uint64_t SIMT_UB_MIN_CACHE_BYTES = 32U * 1024U;
inline constexpr uint64_t SIMT_UB_MAX_CACHE_BYTES = 128U * 1024U;

// Default reserved-space ABI only. Unknown or inconsistent inputs clear layout.valid.
bool BuildSimtUbLayout(SocVersion version, const AclsanUbLaunchMetadata& metadata, AclsanUbLayout& layout) noexcept;

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_UB_LAYOUT_BUILDER_H
