/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef INJECTION_STUBS_RUNTIME_INCLUDE_INJECTION_RUNTIME_STUB_API_H_
#define INJECTION_STUBS_RUNTIME_INCLUDE_INJECTION_RUNTIME_STUB_API_H_

#include "acl/acl_rt.h"
#include "injection/export.h"

#ifdef __cplusplus
extern "C" {
#endif

ACL_TOOL_INJECTION_EXPORT aclError RuntimeStubSetOrigin(const char* name, aclrtApiFunc function);
ACL_TOOL_INJECTION_EXPORT aclError RuntimeStubClearOrigin(const char* name);
ACL_TOOL_INJECTION_EXPORT aclError RuntimeStubSetSocName(const char* socName);
ACL_TOOL_INJECTION_EXPORT void RuntimeStubReset(void);

#ifdef __cplusplus
}

template <typename Function>
aclError RuntimeStubSetOriginFunction(const char* name, Function function)
{
    return RuntimeStubSetOrigin(name, reinterpret_cast<aclrtApiFunc>(function));
}
#endif

#endif // INJECTION_STUBS_RUNTIME_INCLUDE_INJECTION_RUNTIME_STUB_API_H_
