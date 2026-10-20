/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "acl/acl_rt.h"
#include "injection/prof_api_stub.h"
#include "injection/runtime_stub_api.h"

#include <cstring>

TEST(NpuComputeStubStateReset, RestoresDefaultSocName)
{
    RuntimeStubSetSocName("test-soc");
    ASSERT_STREQ(aclrtGetSocName(), "test-soc");
    RuntimeStubReset();
    ASSERT_STREQ(aclrtGetSocName(), "Ascend950PR_9599");
    ProfApiReset();
    RuntimeStubReset();
}
