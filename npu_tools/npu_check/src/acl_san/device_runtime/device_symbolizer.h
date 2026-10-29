/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_RUNTIME_DEVICE_SYMBOLIZER_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_RUNTIME_DEVICE_SYMBOLIZER_H

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aclsan::device_runtime {

struct CallStackFrame {
    std::string functionName;
    std::string fileName;
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t inlineDepth = 0;
};

struct CallStackResult {
    uint64_t pc = 0;
    uint64_t binaryId = 0;
    bool available = false;
    std::string error;
    std::vector<CallStackFrame> frames;
};

struct DeviceSymbolizerConfig {
    std::string symbolizer;
    std::string image;
    std::string workDirectory;
};

using CommandRunner =
    std::function<bool(const std::vector<std::string>& command, const std::string& logPath, std::string& error)>;

class DeviceSymbolizer final {
public:
    explicit DeviceSymbolizer(DeviceSymbolizerConfig config);

    CallStackResult ResolveCallStack(uint64_t pc) const;
    CallStackResult ResolveCallStackWithRunner(uint64_t pc, const CommandRunner& runner) const;
    void Reset();

private:
    struct FrameCacheKey {
        std::string image;
        uint64_t pc = 0;

        bool operator<(const FrameCacheKey& other) const noexcept
        {
            return image < other.image || (image == other.image && pc < other.pc);
        }
    };

    DeviceSymbolizerConfig config_;
    mutable std::mutex mutex_;
    mutable std::map<FrameCacheKey, CallStackResult> cache_;
};

} // namespace aclsan::device_runtime

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_RUNTIME_DEVICE_SYMBOLIZER_H
