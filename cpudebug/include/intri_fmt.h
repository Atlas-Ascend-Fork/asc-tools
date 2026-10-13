/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file intri_fmt.h
 * \brief
 */
#ifndef ASCENDC_INTRI_FMT_H
#define ASCENDC_INTRI_FMT_H
#include <cstdint>
#include "stub_def.h"
#include "stub_fun.h"

#define INTRI_FMT_NUM 5149

struct IntriFmt {
    int32_t fid;
    const char* fmt;
};
using IntriFmtT = IntriFmt;
IntriFmtT* IntriFmtGet(int32_t fid);
#endif // ASCENDC_INTRI_FMT_H
