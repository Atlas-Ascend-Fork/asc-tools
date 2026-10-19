/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_CHECK_MEMORY_ACCESS_STATE_H
#define NPU_CHECK_MEMORY_ACCESS_STATE_H
#include "checker/racecheck/types.h"
#include <functional>
#include <optional>

namespace npucheck::race {
// Stores access indices only. Ordering and reader-frontier pruning belong to Racechecker.
class MemoryAccessState {
public:
    struct State {
        std::optional<size_t> writer;
        std::vector<size_t> readers;
        bool operator==(const State& other) const { return writer == other.writer && readers == other.readers; }
    };
    using StatePtr = std::shared_ptr<const State>;
    struct Update {
        Storage storage;
        Interval range;
        StatePtr state;
    };
    using Visitor = std::function<bool(Interval, const StatePtr&)>;
    MemoryAccessState(size_t segmentBudget, size_t byteBudget) : segmentBudget_(segmentBudget), byteBudget_(byteBudget)
    {}
    bool Visit(const Storage& storage, Interval range, const Visitor& visitor) const;
    // Updates must be sorted by storage/address, nonempty and nonoverlapping.
    // Preallocates affected nodes before modifying any live storage.
    bool Apply(const std::vector<Update>& updates);
    size_t SegmentCount() const { return segments_; }
    size_t AccountedBytes() const { return bytes_; }
    size_t PeakBytes() const { return peak_; }

private:
    struct Segment {
        uint64_t end;
        StatePtr state;
    };
    using Tree = std::map<uint64_t, Segment>;
    static size_t Cost(const StatePtr& state);
    static void Split(Tree& tree, uint64_t position);
    static bool Equal(const StatePtr& a, const StatePtr& b);
    std::map<Storage, Tree> spaces_;
    size_t segmentBudget_, byteBudget_, segments_ = 0, bytes_ = 0, peak_ = 0;
};
} // namespace npucheck::race
#endif
