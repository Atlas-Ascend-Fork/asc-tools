/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef ASCENDC_UT_UTILS_CONST_DEFINER_COMPAT_H
#define ASCENDC_UT_UTILS_CONST_DEFINER_COMPAT_H

// Package-based UTs must use the package's ConstDefiner and matching API types.
// Older CANN packages expose ConstDefiner through kernel_utils.h.
// The standalone kernel_utils_mode_cpu UT uses the repository headers directly.
#include "kernel_utils.h"

#endif // ASCENDC_UT_UTILS_CONST_DEFINER_COMPAT_H
