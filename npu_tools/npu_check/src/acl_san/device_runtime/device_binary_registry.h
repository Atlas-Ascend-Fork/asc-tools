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

#include "aclsan_vf.h"
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

    static constexpr int32_t UNKNOWN_DEVICE = -1;

    bool RecordBinaryLoadFromData(
        uintptr_t binary, bool instrumented, uint32_t traceArgumentOffset, const void* image, size_t imageBytes,
        int32_t deviceId = UNKNOWN_DEVICE) noexcept;
    bool HasKnownBinaryHandle(uintptr_t binary) const noexcept;
    void RecordBinaryUnload(uintptr_t binary, int32_t deviceId = UNKNOWN_DEVICE) noexcept;
    bool RecordLoadedImage(
        uintptr_t binary, const void* image, size_t bytes, int32_t deviceId = UNKNOWN_DEVICE) noexcept;
    bool GetFunctionReadOnlySections(
        uintptr_t function, uintptr_t& binary, std::vector<BinarySectionRange>& sections,
        int32_t deviceId = UNKNOWN_DEVICE) const;
    bool GetFunctionActualBinary(
        uintptr_t function, uintptr_t& binary, std::vector<BinarySegment>& segments,
        int32_t deviceId = UNKNOWN_DEVICE) const noexcept;
    void RecordBinaryFunctionLookup(
        uintptr_t binary, uintptr_t function, const char* functionName = nullptr,
        int32_t deviceId = UNKNOWN_DEVICE) noexcept;
    void RecordLatestBinaryFunctionLookup(uintptr_t function, int32_t deviceId = UNKNOWN_DEVICE) noexcept;
    enum class TraceLookup { Instrumented, NotInstrumented, Unresolved };
    TraceLookup LookupFunctionTraceArgumentOffset(
        uintptr_t function, uint32_t& traceArgumentOffset, int32_t deviceId = UNKNOWN_DEVICE) const noexcept;
    bool GetFunctionTraceArgumentOffset(
        uintptr_t function, uint32_t& traceArgumentOffset, int32_t deviceId = UNKNOWN_DEVICE) const noexcept;
    bool GetFunctionName(
        uintptr_t function, std::string& functionName, int32_t deviceId = UNKNOWN_DEVICE) const noexcept;
    std::shared_ptr<NpubitUbHandle> GetFunctionHibitHandle(
        uintptr_t function, int32_t deviceId = UNKNOWN_DEVICE) const noexcept;
    // Returns retained unknown-device binary and image entries, or SIZE_MAX on error.
    // An unknown device ID never removes entries.
    size_t ResetDevice(int32_t deviceId) noexcept;
    void Reset() noexcept;

    CallStackResult ResolveCallStack(uint64_t pc, int32_t deviceId = UNKNOWN_DEVICE) const noexcept;
    CallStackResult ResolveCallStackWithRunner(
        uint64_t pc, const CommandRunner& runner, int32_t deviceId = UNKNOWN_DEVICE) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace device_runtime

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_RUNTIME_DEVICE_BINARY_REGISTRY_H
