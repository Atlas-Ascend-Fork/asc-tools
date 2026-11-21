/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_COMMUNICATION_INTERVAL_SET_H
#define NPU_COMPUTE_COMMUNICATION_INTERVAL_SET_H
#include <algorithm>
#include <vector>
namespace npucompute::communication {
// Nanoseconds on a calibrated common clock; half-open intervals [start,end).
struct Interval {
    long double start = 0, end = 0;
};
using Intervals = std::vector<Interval>;
// Inputs must already be sorted, disjoint unions.
inline Intervals Intersection(const Intervals& a, const Intervals& b)
{
    Intervals result;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const auto start = std::max(a[i].start, b[j].start), end = std::min(a[i].end, b[j].end);
        if (start < end)
            result.push_back({start, end});
        if (a[i].end < b[j].end)
            ++i;
        else
            ++j;
    }
    return result;
}
inline long double Duration(const Intervals& values)
{
    long double result = 0;
    for (const auto& value : values)
        result += value.end - value.start;
    return result;
}
} // namespace npucompute::communication
#endif
