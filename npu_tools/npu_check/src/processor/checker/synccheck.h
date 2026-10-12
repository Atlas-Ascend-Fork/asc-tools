/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_SYNCCHECK_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_SYNCCHECK_H

#include "acl_san/aclsan_cbdata_device.h"
#include "diagnostic/report_renderer.h"
#include "checker/checker.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace npucheck {

struct SynccheckStats {
    uint64_t syncEvents = 0;
    uint64_t synchronizationEvents = 0;
    uint64_t matchedPairs = 0;
    uint64_t duplicateOpens = 0;
    uint64_t unmatchedCloses = 0;
    uint64_t unconsumedOpens = 0;
    uint64_t pendingOpens = 0;
};

class Synccheck final : public npucheck::Checker {
public:
    const std::vector<npucheck::CallbackSpec>& GetSubscribedID() const override;
    bool OnCallback(
        AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data,
        npucheck::CheckerReportList& reports) override;
    std::string Summary() const override;
    bool HasErrors() const override;
    bool AnalysisComplete() const override;
    void OnDeviceSync(const AclsanDeviceSyncData& data);
    std::vector<npucheck::NpuCheckSynccheckReport> OnSynchronization();
    SynccheckStats Stats() const;

private:
    static void HashCombine(size_t& seed, uint64_t value) noexcept;

    struct FlagPairKey {
        uint64_t objectId = 0;
        uint32_t phyCoreId = 0;
        uint32_t blockId = 0;
        uint32_t srcPipe = 0;
        uint32_t dstPipe = 0;

        static FlagPairKey FromCbdata(const AclsanDeviceSyncData& data) noexcept
        {
            return {data.objectId, data.header.phyCoreId, data.header.blockId, data.srcPipe, data.dstPipe};
        }

        bool operator==(const FlagPairKey& other) const noexcept
        {
            return objectId == other.objectId && phyCoreId == other.phyCoreId && blockId == other.blockId &&
                   srcPipe == other.srcPipe && dstPipe == other.dstPipe;
        }
    };

    struct FlagPairKeyHash {
        size_t operator()(const FlagPairKey& key) const noexcept
        {
            size_t seed = 0;
            HashCombine(seed, key.objectId);
            HashCombine(seed, key.phyCoreId);
            HashCombine(seed, key.blockId);
            HashCombine(seed, key.srcPipe);
            HashCombine(seed, key.dstPipe);
            return seed;
        }
    };

    struct SyncBufPairKey {
        uint64_t objectId = 0;
        uint32_t phyCoreId = 0;
        uint32_t blockId = 0;
        uint32_t pipe = 0;
        uint32_t mode = 0;

        static SyncBufPairKey FromCbdata(const AclsanDeviceSyncData& data) noexcept
        {
            return {data.objectId, data.header.phyCoreId, data.header.blockId, data.dstPipe, data.mode};
        }

        bool operator==(const SyncBufPairKey& other) const noexcept
        {
            return objectId == other.objectId && phyCoreId == other.phyCoreId && blockId == other.blockId &&
                   pipe == other.pipe && mode == other.mode;
        }
    };

    struct SyncBufPairKeyHash {
        size_t operator()(const SyncBufPairKey& key) const noexcept
        {
            size_t seed = 0;
            HashCombine(seed, key.objectId);
            HashCombine(seed, key.phyCoreId);
            HashCombine(seed, key.blockId);
            HashCombine(seed, key.pipe);
            HashCombine(seed, key.mode);
            return seed;
        }
    };

    struct SyncBufOccupancyKey {
        uint64_t objectId = 0;
        uint32_t phyCoreId = 0;

        bool operator==(const SyncBufOccupancyKey& other) const noexcept
        {
            return objectId == other.objectId && phyCoreId == other.phyCoreId;
        }
    };

    struct SyncBufOccupancyKeyHash {
        size_t operator()(const SyncBufOccupancyKey& key) const noexcept
        {
            size_t seed = 0;
            HashCombine(seed, key.objectId);
            HashCombine(seed, key.phyCoreId);
            return seed;
        }
    };

    using FlagPendingMap = std::unordered_map<FlagPairKey, AclsanDeviceSyncData, FlagPairKeyHash>;
    using SyncBufPendingMap = std::unordered_map<SyncBufPairKey, AclsanDeviceSyncData, SyncBufPairKeyHash>;
    using ActiveSyncBufMap = std::unordered_map<SyncBufOccupancyKey, SyncBufPairKey, SyncBufOccupancyKeyHash>;
    using Report = npucheck::NpuCheckSynccheckReport;
    using Reports = std::vector<Report>;

    struct FlagState {
        FlagPendingMap pending;
    };

    struct SyncBufState {
        SyncBufPendingMap pending;
        ActiveSyncBufMap activeSyncBufs;
    };

    struct LaunchSyncState {
        uint64_t launchId = 0;
        mutable FlagState flags;
        mutable SyncBufState syncBufs;
        mutable Reports reports;

        bool operator==(const LaunchSyncState& other) const noexcept { return launchId == other.launchId; }
    };

    struct LaunchSyncStateHash {
        size_t operator()(const LaunchSyncState& state) const noexcept { return std::hash<uint64_t>{}(state.launchId); }
    };

    static SyncBufOccupancyKey BuildSyncBufOccupancyKey(const AclsanDeviceSyncData& data);
    static Report BuildMismatchBase(
        const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason);
    static Report BuildMismatch(
        const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason,
        const FlagPairKey& key);
    static Report BuildMismatch(
        const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason,
        const SyncBufPairKey& key);
    static npucheck::NpuCheckSyncPoint ActualPoint(const AclsanDeviceSyncData& data);
    static npucheck::NpuCheckSyncPoint ExpectedPoint(const AclsanDeviceSyncData& data, uint32_t reason);
    void HandleFlagEvent(FlagState& state, const AclsanDeviceSyncData& data, const FlagPairKey& key, Reports& reports);
    void HandleSyncBufEvent(
        SyncBufState& state, const AclsanDeviceSyncData& data, const SyncBufPairKey& key, Reports& reports);

    std::unordered_set<LaunchSyncState, LaunchSyncStateHash> launchStates_;
    SynccheckStats stats_{};
};

} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_SYNCCHECK_H
