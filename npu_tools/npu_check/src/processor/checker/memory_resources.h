/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_CHECK_MEMORY_RESOURCES_H
#define NPU_CHECK_MEMORY_RESOURCES_H
#include "acl_san/aclsan_api.h"
#include "checker/host_order.h"
#include "checker/memory_state.h"
#include <optional>

namespace npucheck {
struct MemoryLocation {
    uint32_t device;
    uint64_t address;
    uint64_t bytes;
};

// Lifecycle queries over the single allocation ledger owned by MemoryState.
// The ledger stores every allocation once; this class only applies resource
// callbacks and answers which generation was live at a point in the host order.
class MemoryResources {
public:
    AllocationUpdateStatus Apply(AclsanCallbackId id, const AclsanResourceData& data, MemoryState& state, size_t node);
    RangeResult Resolve(
        const MemoryLocation& location, size_t entry, size_t exit, HostOrder& order, MemoryState& state);
    bool Complete() const { return complete_; }
    size_t RecordsScanned() const { return queries_; }

private:
    size_t queries_ = 0;
    bool complete_ = true;
};
} // namespace npucheck
#endif
