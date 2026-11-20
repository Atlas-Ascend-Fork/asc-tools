/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_VECTOR_CBDATA_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_VECTOR_CBDATA_H

#include "aclsan_memory_cbdata.h"

namespace aclsan {

MemoryCbdataResult ConvertVtranspose(const VtransposeParamField& field, const MemoryCbdataContext& context) noexcept;
MemoryCbdataResult ConvertVbs32(const Vbs32ParamField& field, const MemoryCbdataContext& context) noexcept;
MemoryCbdataResult ConvertVmrgsort(const VmrgsortParamField& field, const MemoryCbdataContext& context) noexcept;
MemoryCbdataResult ConvertVnchwconv(
    const VnchwconvParamField& field, const MemoryCbdataContext& context, const MemoryRegisterState& state) noexcept;
MemoryCbdataResult ConvertLdva(const LdvaParamField& field, const MemoryCbdataContext& context) noexcept;

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_VECTOR_CBDATA_H
