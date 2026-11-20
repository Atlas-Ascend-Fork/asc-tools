/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#pragma once
#include "injection/injection_hook.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace aclpti::profiling {
struct SimtWarpBinarySource {
    std::shared_ptr<const std::vector<char>> image;
    std::vector<aclrtBinaryLoadOption> options;
    std::string name;
    std::uint64_t entry = 0;
};
// 独立管理原始 ELF 和函数名称。接入 Runtime Hook 时调用 Register*。
// 示例只支持按函数名定位；entry-only 应显式报错，不能猜测插桩后地址。
class SimtWarpSourceRegistry {
public:
    bool RegisterBinary(
        aclrtBinHandle binary, const void* data, std::size_t size, const aclrtBinaryLoadOptions* options);
    bool RegisterFunction(aclrtBinHandle binary, aclrtFuncHandle function, const char* name);
    bool GetSource(aclrtFuncHandle function, SimtWarpBinarySource& source);
    void UnregisterBinary(aclrtBinHandle binary);

private:
    struct Entry {
        SimtWarpBinarySource source;
        std::unordered_map<aclrtFuncHandle, std::string> names;
    };
    std::mutex mutex_;
    std::unordered_map<aclrtBinHandle, Entry> binaries_;
};
} // namespace aclpti::profiling
