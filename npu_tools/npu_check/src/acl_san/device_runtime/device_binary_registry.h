/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_RUNTIME_DEVICE_BINARY_REGISTRY_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_RUNTIME_DEVICE_BINARY_REGISTRY_H

#include "device_symbolizer.h"
#include "dbi/kernel_param_metadata.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aclsan {

struct BinarySegment {
    uint64_t offset = 0;
    uint64_t bytes = 0;
    uint32_t flags = 0;
};

namespace device_runtime {

class DeviceBinaryRegistry final {
public:
    DeviceBinaryRegistry();
    ~DeviceBinaryRegistry();

    DeviceBinaryRegistry(const DeviceBinaryRegistry&) = delete;
    DeviceBinaryRegistry& operator=(const DeviceBinaryRegistry&) = delete;

    bool RecordBinaryLoadFromData(
        uintptr_t binary, bool instrumented, uint32_t traceArgumentOffset, const void* image,
        size_t imageBytes) noexcept;
    void RecordBinaryUnload(uintptr_t binary) noexcept;
    bool RecordLoadedImage(uintptr_t binary, const void* image, size_t bytes) noexcept;
    bool GetFunctionReadOnlySections(
        uintptr_t function, uintptr_t& binary, std::vector<BinarySectionRange>& sections) const;
    bool GetFunctionActualBinary(
        uintptr_t function, uintptr_t& binary, std::vector<BinarySegment>& segments) const noexcept;
    void RecordBinaryFunctionLookup(uintptr_t binary, uintptr_t function, const char* functionName = nullptr) noexcept;
    void RecordLatestBinaryFunctionLookup(uintptr_t function) noexcept;
    bool GetFunctionTraceArgumentOffset(uintptr_t function, uint32_t& traceArgumentOffset) const noexcept;
    bool GetFunctionName(uintptr_t function, std::string& functionName) const noexcept;
    void Reset() noexcept;

    CallStackResult ResolveCallStack(uint64_t pc) const noexcept;
    CallStackResult ResolveCallStackWithRunner(uint64_t pc, const CommandRunner& runner) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace device_runtime

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_RUNTIME_DEVICE_BINARY_REGISTRY_H
