/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "mock.h"
#include <algorithm>
#include <cassert>
#include <vector>
struct Handle {
    const uint8_t* object;
    std::vector<uint8_t> original;
};
extern "C" NpubitStatus NpubitCreateHandle(
    const void* object, size_t size, NpubitHandle* out, NpubitPlatformType platform, bool verify)
{
    assert(platform == V100 && verify && size && object);
    if (hibit_test::createStatus) {
        return hibit_test::createStatus;
    }
    const auto* bytes = static_cast<const uint8_t*>(object);
    *out = new Handle{bytes, {bytes, bytes + size}};
    ++hibit_test::creates;
    return 0;
}
extern "C" void NpubitReleaseHandle(NpubitHandle opaque)
{
    const auto* h = static_cast<Handle*>(opaque);
    assert(std::equal(h->original.begin(), h->original.end(), h->object));
    delete h;
    ++hibit_test::destroys;
}
extern "C" NpubitStatus NpubitGetUbAddressRanges(
    NpubitHandle opaque, const NpubitPushPBStateRecord* input, NpubitUbAddressRange** out, size_t* count)
{
    const auto* h = static_cast<Handle*>(opaque);
    assert(std::equal(h->original.begin(), h->original.end(), h->object));
    hibit_test::queries.push_back(*input);
    *count = hibit_test::ranges.size();
    if (*count) {
        *out = new NpubitUbAddressRange[*count];
        std::copy(hibit_test::ranges.begin(), hibit_test::ranges.end(), *out);
    }
    return hibit_test::queryStatus;
}
extern "C" void NpubitFree(void** data)
{
    delete[] static_cast<NpubitUbAddressRange*>(*data);
    *data = nullptr;
    ++hibit_test::frees;
}
