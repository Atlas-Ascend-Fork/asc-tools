/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_VF_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_VF_H

#include "aclsan_trace_buffer.h"
#include "acl_san/aclsan_api.h"
#include "device_instr/common/device_instr_struct_register.h"
#include "device_instr/common/device_instr_struct_vector.h"

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <variant>
#include <vector>

namespace aclsan {

// One instance per launch; the key also prevents accidental mixing of launches in direct dispatch.
class VfPbState final {
public:
    bool Push(const ParsedTraceRecord& record, const std::array<uint64_t, PUSH_PB_VALUE_COUNT>& values);
    std::optional<std::array<uint64_t, PUSH_PB_STATE_COUNT_PER_VF>> Consume(const ParsedTraceRecord& record);

private:
    struct State {
        std::array<uint64_t, PUSH_PB_STATE_COUNT_PER_VF> values{};
        std::size_t count = 0;
        bool consumed = false;
        bool invalid = false;
    };
    using Key = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    static Key Identity(const ParsedTraceRecord& record);
    std::map<Key, State> states_;
};

// Parent context is copied once; internal callbacks keep exactly this trace identity.
inline AclsanDeviceVfData MakeVfCompletion(const ParsedTraceRecord& vf, uint32_t status)
{
    AclsanDeviceVfData data{};
    data.header = {
        ACLSAN_API_VERSION,
        sizeof(data),
        vf.launchId,
        vf.record.pc,
        vf.record.siteId,
        ACLSAN_DEVICE_SOURCE_VECTOR,
        vf.instrExecId,
        0,
        vf.deviceId,
        vf.phyCoreId,
        vf.blockId,
        vf.blockType,
        ACLSAN_DEVICE_PIPE_VECTOR,
        ACLSAN_DEVICE_EVENT_FLAG_EXACT};
    data.status = status;
    return data;
}

using VfCallbackData = std::variant<AclsanDeviceMemoryAccessData, AclsanDeviceBarrierData>;

// Vendor ABI is confined to the implementation. Object storage outlives the vendor handle.
class NpubitUbHandle final {
private:
    struct Impl;

public:
    static std::shared_ptr<NpubitUbHandle> Create(const void* object, size_t bytes) noexcept;
    ~NpubitUbHandle();
    explicit NpubitUbHandle(std::unique_ptr<Impl> impl);
    bool Matches(const void* object, size_t bytes) const noexcept;
    // Preserve vendor record order across UB accesses and local-memory barriers.
    std::vector<VfCallbackData> Query(
        const ParsedTraceRecord& vf, const std::array<uint64_t, PUSH_PB_STATE_COUNT_PER_VF>& states,
        const std::array<uint64_t, VECTOR_MASK_REGISTER_COUNT>& masks,
        AclsanDeviceVfData* completion = nullptr) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_VF_H
