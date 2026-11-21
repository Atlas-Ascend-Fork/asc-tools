/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "communication/interval_set.h"
#include <stdexcept>
using namespace npucompute::communication;
void Check(bool v)
{
    if (!v)
        throw std::runtime_error("interval intersection/duration mismatch");
}
int main()
{
    Check(Duration({}) == 0);
    Check(Duration({{0, 30}}) == 30);
    Check(Duration(Intersection({{0, 10}}, {{20, 30}})) == 0);
    Check(Duration(Intersection({{20, 30}}, {{0, 10}})) == 0);
    Check(Duration(Intersection({{0, 30}}, {{5, 25}})) == 20);
    Check(Duration(Intersection({{0, 8}, {20, 35}}, {{5, 25}})) == 8);
    Check(Duration(Intersection({{0, 10}}, {{10, 20}})) == 0);
    Check(Duration(Intersection({}, {{0, 10}})) == 0);
}
