// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#ifndef NPU_TOOLS_NPU_CHECK_INITCHECK_MEMORY_H
#define NPU_TOOLS_NPU_CHECK_INITCHECK_MEMORY_H

#include "checker/checker.h"
#include <map>
#include <tuple>
#include <variant>

namespace npucheck {

// Tracks initialized L0A/L0B bytes per execution context and checks reads
// against preceding writes and synchronization. Reports contain the results
// of this analysis; formatting and output use the existing report pipeline.
class InitcheckMemory {
public:
    void Launch(const AclsanLaunchData& data);
    void Access(const AclsanDeviceMemoryAccessData& data);
    // Callback headers must have passed Initcheck::OnDeviceSync version/size checks.
    void Sync(const AclsanDeviceSyncData& data);
    void Barrier(const AclsanDeviceBarrierData& data);
    void Collect(const void* stream, CheckerReportList& reports);
    void Finalize();
    void Invalidate()
    {
        ++incomplete_;
        invalid_ = true;
    }
    bool Complete() const { return incomplete_ == 0 && pending_.empty(); }
    uint64_t IncompleteOperations() const { return incomplete_; }
    uint64_t Operations() const { return operations_; }

private:
    using CoreKey = std::tuple<uint32_t, uint64_t, uint32_t, uint32_t>;
    using Event = std::variant<AclsanDeviceMemoryAccessData, AclsanDeviceSyncData, AclsanDeviceBarrierData>;
    void Queue(const AclsanDeviceEventHeader& header, Event event);
    void Replay(const std::vector<Event>& records, CheckerReportList& reports);
    std::map<CoreKey, std::vector<Event>> pending_;
    std::map<uint64_t, void*> streams_;
    size_t queued_ = 0;
    bool invalid_ = false;
    uint64_t incomplete_ = 0;
    uint64_t operations_ = 0;
};
} // namespace npucheck
#endif
