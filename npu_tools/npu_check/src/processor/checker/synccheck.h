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
#include "checker/checker.h"
#include "diagnostic/report_renderer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace npucheck {

enum class KernelMixType : uint32_t { UNKNOWN = 0, AIC_ONLY, AIV_ONLY, MIX_1_1, MIX_1_2 };
enum class ParticipantRole : uint32_t { UNKNOWN = 0, AIC, AIV0, AIV1 };

struct LaunchTopology {
    uint32_t kernelType = 0;
    uint16_t aicRatio = 0;
    uint16_t aivRatio = 0;
    uint32_t groupCount = 0;
    KernelMixType mixType = KernelMixType::UNKNOWN;
    uint32_t aicPerGroup = 0;
    uint32_t aivPerGroup = 0;
    uint32_t expectedAicCount = 0;
    uint32_t expectedAivCount = 0;
    uint32_t schedMode = 0;
    bool kernelTypeValid = false;
    bool ratioValid = false;
    bool groupCountValid = false;
};

struct SynccheckStats {
    uint64_t syncEvents = 0;
    uint64_t synchronizationEvents = 0;
    uint64_t matchedPairs = 0;
    uint64_t duplicateOpens = 0;
    uint64_t unmatchedCloses = 0;
    uint64_t unconsumedOpens = 0;
    uint64_t pendingOpens = 0;
    uint64_t matchedCrossRounds = 0;
    uint64_t producedCrossTokens = 0;
    uint64_t consumedCrossTokens = 0;
    uint64_t unconsumedSyncTokens = 0;
    uint64_t unconsumedSyncTokenReports = 0;
    uint64_t waitWithoutSyncTokens = 0;
    uint64_t counterOverflows = 0;
    uint64_t participantMismatches = 0;
    uint64_t sequenceMismatches = 0;
    uint64_t invalidArguments = 0;
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
    void OnKernelLaunch(uint64_t launchId, const LaunchTopology& topology, void* stream = nullptr);
    void OnDeviceSync(const AclsanDeviceSyncData& data);
    std::vector<npucheck::NpuCheckSynccheckReport> OnSynchronization();
    std::vector<npucheck::NpuCheckSynccheckReport> OnSynchronization(void* stream);
    SynccheckStats Stats() const;
    std::optional<LaunchTopology> TopologyForLaunch(uint64_t launchId) const;
    std::optional<uint32_t> CrossCounterValue(
        uint64_t launchId, uint32_t groupId, ParticipantRole role, uint32_t flagId) const;
    size_t PendingCrossSetRoundCount(uint64_t launchId) const;

private:
    static constexpr uint32_t kGlobalGroup = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t kInvalidAivIndex = std::numeric_limits<uint32_t>::max();
    static void HashCombine(size_t& seed, uint64_t value) noexcept;

    struct FlagPairKey {
        uint64_t objectId = 0;
        uint32_t blockType = 0;
        uint32_t blockId = 0;
        uint32_t srcPipe = 0;
        uint32_t dstPipe = 0;
        static FlagPairKey FromCbdata(const AclsanDeviceSyncData& data) noexcept;
        bool operator==(const FlagPairKey& other) const noexcept;
    };
    struct FlagPairKeyHash {
        size_t operator()(const FlagPairKey& key) const noexcept;
    };

    struct SyncBufPairKey {
        uint64_t objectId = 0;
        uint32_t blockType = 0;
        uint32_t blockId = 0;
        uint32_t pipe = 0;
        uint32_t mode = 0;
        static SyncBufPairKey FromCbdata(const AclsanDeviceSyncData& data) noexcept;
        bool operator==(const SyncBufPairKey& other) const noexcept;
    };
    struct SyncBufPairKeyHash {
        size_t operator()(const SyncBufPairKey& key) const noexcept;
    };

    struct SyncBufOccupancyKey {
        uint64_t objectId = 0;
        uint32_t phyCoreId = 0;
        bool operator==(const SyncBufOccupancyKey& other) const noexcept;
    };
    struct SyncBufOccupancyKeyHash {
        size_t operator()(const SyncBufOccupancyKey& key) const noexcept;
    };

    struct ParticipantKey {
        uint32_t groupId = 0;
        ParticipantRole role = ParticipantRole::UNKNOWN;
        bool operator==(const ParticipantKey& other) const noexcept;
    };
    struct ParticipantKeyHash {
        size_t operator()(const ParticipantKey& key) const noexcept;
    };
    enum class CrossDirection : uint32_t { UNKNOWN = 0, ALL_AIC, ALL_AIV, AIV_TO_AIV, AIC_TO_AIV, AIV_TO_AIC };
    struct CounterKey {
        ParticipantKey target;
        uint32_t flagId = 0;
        bool operator==(const CounterKey& other) const noexcept;
    };
    struct CounterKeyHash {
        size_t operator()(const CounterKey& key) const noexcept;
    };

    struct SetChannelKey {
        uint32_t mode = 0;
        uint32_t flagId = 0;       // target-local counter id
        uint32_t sourceFlagId = 0; // source-local hardware flag id
        uint32_t groupId = 0;
        CrossDirection direction = CrossDirection::UNKNOWN;
        ParticipantKey target;
        bool operator==(const SetChannelKey& other) const noexcept;
    };
    struct SetChannelKeyHash {
        size_t operator()(const SetChannelKey& key) const noexcept;
    };

    struct FlagUsageKey {
        ParticipantKey participant;
        uint32_t flagId = 0;
        bool operator==(const FlagUsageKey& other) const noexcept;
    };
    struct FlagUsageKeyHash {
        size_t operator()(const FlagUsageKey& key) const noexcept;
    };

    using ParticipantSet = std::unordered_set<ParticipantKey, ParticipantKeyHash>;
    using OccurrenceMap = std::unordered_map<ParticipantKey, uint64_t, ParticipantKeyHash>;
    struct SetRound {
        ParticipantSet expectedSet;
        ParticipantSet actualSet;
        AclsanDeviceSyncData firstEvent{};
    };
    struct SetChannelState {
        std::unordered_map<uint64_t, SetRound> pendingRounds;
        OccurrenceMap setOccurrences;
    };

    struct CounterState {
        uint32_t value = 0;
        std::deque<AclsanDeviceSyncData> pendingWaits;
        AclsanDeviceSyncData lastProducer{};
    };
    struct FlagUsageState {
        uint32_t activeMode = ACLSAN_DEVICE_SYNC_MODE_UNKNOWN;
        uint32_t pendingSetRoundRefs = 0;
    };
    struct CounterContribution {
        CounterKey targetCounter;
        ParticipantSet requiredSetParticipants;
        uint32_t sourceFlagId = 0;
        uint32_t mode = ACLSAN_DEVICE_SYNC_MODE_UNKNOWN;
        uint32_t groupId = 0;
        CrossDirection direction = CrossDirection::UNKNOWN;
    };
    struct CrossCoreState {
        std::unordered_map<SetChannelKey, SetChannelState, SetChannelKeyHash> pendingSetRounds;
        std::unordered_map<CounterKey, CounterState, CounterKeyHash> counters;
        std::unordered_map<FlagUsageKey, FlagUsageState, FlagUsageKeyHash> flagUsage;
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
        void* stream = nullptr;
        LaunchTopology topology;
        FlagState flags;
        SyncBufState syncBufs;
        CrossCoreState crossCore;
        Reports reports;
    };
    struct CanonicalCrossFlag {
        uint32_t sourceFlagId = 0;
        uint32_t targetFlagId = 0;
        uint32_t aivIndex = 0;
    };

    static LaunchTopology NormalizeTopology(const LaunchTopology& topology);
    static SyncBufOccupancyKey BuildSyncBufOccupancyKey(const AclsanDeviceSyncData& data);
    static bool ResolveParticipant(
        const LaunchTopology& topology, const AclsanDeviceSyncData& data, ParticipantKey& participant,
        std::string& error);
    static CanonicalCrossFlag CanonicalizeMode4SetFlag(const ParticipantKey& participant, uint64_t rawFlagId);
    static uint32_t CanonicalizeLocalWaitFlag(const ParticipantKey& participant, uint32_t syncKind, uint64_t rawFlagId);
    static bool ValidateCrossInstruction(const AclsanDeviceSyncData& data, std::string& error);
    static bool BuildCounterContributions(
        const LaunchSyncState& state, const ParticipantKey& participant, uint32_t mode, uint32_t sourceFlagId,
        uint32_t targetFlagId, uint32_t aivIndex, std::vector<CounterContribution>& contributions, std::string& error);
    static std::string ParticipantSetText(const ParticipantSet& participants);
    static const char* DirectionName(CrossDirection direction);

    void ProcessSyncEvent(LaunchSyncState& state, const AclsanDeviceSyncData& data);
    void HandleFlagEvent(FlagState& state, const AclsanDeviceSyncData& data, const FlagPairKey& key, Reports& reports);
    void HandleSyncBufEvent(
        SyncBufState& state, const AclsanDeviceSyncData& data, const SyncBufPairKey& key, Reports& reports);
    void HandleCrossCoreEvent(LaunchSyncState& state, const AclsanDeviceSyncData& data);
    void ProcessCrossSet(
        LaunchSyncState& state, const ParticipantKey& participant, const AclsanDeviceSyncData& data,
        const CanonicalCrossFlag& flag);
    void ProcessCrossWait(
        LaunchSyncState& state, const ParticipantKey& participant, const AclsanDeviceSyncData& data, uint32_t flagId);
    bool ValidateFlagMode(
        LaunchSyncState& state, const FlagUsageKey& key, uint32_t mode, const AclsanDeviceSyncData& trigger);
    void ActivateSetRoundUsage(LaunchSyncState& state, const SetChannelKey& key, const SetRound& round);
    void ReleaseSetRoundUsage(LaunchSyncState& state, const SetChannelKey& key, const SetRound& round);
    void ProduceCounterToken(
        LaunchSyncState& state, const CounterContribution& contribution, const AclsanDeviceSyncData& trigger);
    void CleanupFlagUsage(LaunchSyncState& state, const FlagUsageKey& key);
    void FinalizeCrossCore(LaunchSyncState& state, Reports& reports);
    Reports FinalizeLaunches(void* stream, bool filterByStream);

    static Report BuildMismatchBase(
        const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason);
    static Report BuildReportBase(
        const AclsanDeviceSyncData& trigger, NpuCheckReportPattern pattern, NpuCheckSyncPrimitiveKind primitiveKind,
        NpuCheckSyncDetailKind detailKind);
    static Report BuildMismatch(
        const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason,
        const FlagPairKey& key);
    static Report BuildMismatch(
        const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason,
        const SyncBufPairKey& key);
    static Report BuildInvalid(const AclsanDeviceSyncData& trigger, const std::string& reason);
    static Report BuildSequence(const AclsanDeviceSyncData& trigger, const std::string& reason);
    static Report BuildParticipantMismatch(
        const SetChannelKey& key, uint64_t roundId, const SetRound& round, const AclsanDeviceSyncData& trigger);
    static Report BuildCrossTokenMismatch(
        const CounterKey& key, const CounterState& counter, const AclsanDeviceSyncData& trigger, uint32_t reason,
        uint32_t tokenCount);
    static npucheck::NpuCheckSyncPoint ActualPoint(const AclsanDeviceSyncData& data);
    static npucheck::NpuCheckSyncPoint ExpectedPoint(const AclsanDeviceSyncData& data, uint32_t reason);

    std::unordered_map<uint64_t, LaunchSyncState> launchStates_;
    Reports orphanReports_;
    std::unordered_set<uint64_t> finalizedLaunches_;
    SynccheckStats stats_{};
};

} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_SYNCCHECK_H
