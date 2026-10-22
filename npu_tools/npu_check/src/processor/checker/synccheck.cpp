/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/synccheck.h"
#include "diagnostic/pipeline_name.h"

#include <algorithm>
#include <functional>
#include <sstream>
#include <tuple>
#include <utility>

namespace npucheck {
namespace {

enum class AclKernelType : uint32_t {
    AICORE = 0,
    CUBE = 1,
    VECTOR = 2,
    MIX = 3,
};

bool IsCrossCoreSyncKind(uint32_t syncKind)
{
    return syncKind == ACLSAN_DEVICE_SYNC_KIND_CROSS_CORE || syncKind == ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK;
}

const char* CrossInstructionName(uint32_t syncKind, uint32_t action)
{
    if (syncKind == ACLSAN_DEVICE_SYNC_KIND_CROSS_CORE) {
        return action == ACLSAN_DEVICE_SYNC_ACTION_SET ? "FFTS_CROSS_CORE_SYNC" : "WAIT_FLAG_DEV";
    }
    if (syncKind == ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK) {
        return action == ACLSAN_DEVICE_SYNC_ACTION_SET ? "SET_INTRA_BLOCK" : "WAIT_INTRA_BLOCK";
    }
    return nullptr;
}

const char* OperationName(const AclsanDeviceSyncData& data)
{
    if (data.syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG) {
        return data.action == ACLSAN_DEVICE_SYNC_ACTION_SET ? "SET_FLAG" : "WAIT_FLAG";
    }
    if (data.syncKind == ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF) {
        return data.action == ACLSAN_DEVICE_SYNC_ACTION_GET ? "GET_BUF" : "RLS_BUF";
    }
    if (IsCrossCoreSyncKind(data.syncKind)) {
        const char* instruction = CrossInstructionName(data.syncKind, data.action);
        return instruction == nullptr ?
                   (data.action == ACLSAN_DEVICE_SYNC_ACTION_SET ? "CROSS_CORE_SET" : "CROSS_CORE_WAIT") :
                   instruction;
    }
    return "UNKNOWN_SYNC";
}

const char* ExpectedOperation(const AclsanDeviceSyncData& data, uint32_t reason)
{
    const bool unmatched = reason == static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE);
    if (data.syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG) {
        return unmatched ? "SET_FLAG" : "WAIT_FLAG";
    }
    if (IsCrossCoreSyncKind(data.syncKind)) {
        const uint32_t expectedAction = unmatched ? ACLSAN_DEVICE_SYNC_ACTION_SET : ACLSAN_DEVICE_SYNC_ACTION_WAIT;
        const char* instruction = CrossInstructionName(data.syncKind, expectedAction);
        return instruction == nullptr ? (unmatched ? "CROSS_CORE_SET" : "CROSS_CORE_WAIT") : instruction;
    }
    return unmatched ? "GET_BUF" : "RLS_BUF";
}

NpuCheckReportExecContext ExecContext(const AclsanDeviceSyncData& data)
{
    NpuCheckReportExecContext exec{};
    exec.launchId = data.header.launchId;
    exec.instrExecId = data.header.instrExecId;
    exec.pc = data.header.pc;
    exec.phyCoreId = data.header.phyCoreId;
    exec.blockId = data.header.blockId;
    exec.blockType = data.header.blockType;
    // Sync instructions execute on PIPE_S independently of their srcPipe/dstPipe pairing arguments.
    exec.pipeId = 0;
    exec.pipeName = detail::DevicePipelineName(ACLSAN_DEVICE_PIPE_SCALAR);
    return exec;
}

template <typename Set>
bool SetEquals(const Set& left, const Set& right)
{
    return left.size() == right.size() &&
           std::all_of(left.begin(), left.end(), [&right](const auto& value) { return right.count(value) != 0; });
}

bool IsMode4WaitInstruction(const AclsanDeviceSyncData& data)
{
    return data.syncKind == ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK && data.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT;
}

const char* KernelMixTypeName(KernelMixType mixType)
{
    switch (mixType) {
        case KernelMixType::AIC_ONLY:
            return "AIC_ONLY";
        case KernelMixType::AIV_ONLY:
            return "AIV_ONLY";
        case KernelMixType::MIX_1_1:
            return "MIX_1_1";
        case KernelMixType::MIX_1_2:
            return "MIX_1_2";
        default:
            return "UNKNOWN";
    }
}

} // namespace

void Synccheck::HashCombine(size_t& seed, uint64_t value) noexcept
{
    seed ^= std::hash<uint64_t>{}(value) + static_cast<size_t>(0x9e3779b9U) + (seed << 6U) + (seed >> 2U);
}

Synccheck::FlagPairKey Synccheck::FlagPairKey::FromCbdata(const AclsanDeviceSyncData& data) noexcept
{
    return {data.objectId, data.header.blockType, data.header.blockId, data.srcPipe, data.dstPipe};
}

bool Synccheck::FlagPairKey::operator==(const FlagPairKey& other) const noexcept
{
    return objectId == other.objectId && blockType == other.blockType && blockId == other.blockId &&
           srcPipe == other.srcPipe && dstPipe == other.dstPipe;
}

size_t Synccheck::FlagPairKeyHash::operator()(const FlagPairKey& key) const noexcept
{
    size_t seed = 0;
    HashCombine(seed, key.objectId);
    HashCombine(seed, key.blockType);
    HashCombine(seed, key.blockId);
    HashCombine(seed, key.srcPipe);
    HashCombine(seed, key.dstPipe);
    return seed;
}

Synccheck::SyncBufPairKey Synccheck::SyncBufPairKey::FromCbdata(const AclsanDeviceSyncData& data) noexcept
{
    return {data.objectId, data.header.blockType, data.header.blockId, data.dstPipe, data.mode};
}

bool Synccheck::SyncBufPairKey::operator==(const SyncBufPairKey& other) const noexcept
{
    return objectId == other.objectId && blockType == other.blockType && blockId == other.blockId &&
           pipe == other.pipe && mode == other.mode;
}

size_t Synccheck::SyncBufPairKeyHash::operator()(const SyncBufPairKey& key) const noexcept
{
    size_t seed = 0;
    HashCombine(seed, key.objectId);
    HashCombine(seed, key.blockType);
    HashCombine(seed, key.blockId);
    HashCombine(seed, key.pipe);
    HashCombine(seed, key.mode);
    return seed;
}

bool Synccheck::SyncBufOccupancyKey::operator==(const SyncBufOccupancyKey& other) const noexcept
{
    return objectId == other.objectId && phyCoreId == other.phyCoreId;
}

size_t Synccheck::SyncBufOccupancyKeyHash::operator()(const SyncBufOccupancyKey& key) const noexcept
{
    size_t seed = 0;
    HashCombine(seed, key.objectId);
    HashCombine(seed, key.phyCoreId);
    return seed;
}

bool Synccheck::ParticipantKey::operator==(const ParticipantKey& other) const noexcept
{
    return groupId == other.groupId && role == other.role;
}

size_t Synccheck::ParticipantKeyHash::operator()(const ParticipantKey& key) const noexcept
{
    size_t seed = 0;
    HashCombine(seed, key.groupId);
    HashCombine(seed, static_cast<uint32_t>(key.role));
    return seed;
}

bool Synccheck::CounterKey::operator==(const CounterKey& other) const noexcept
{
    return target == other.target && flagId == other.flagId;
}

size_t Synccheck::CounterKeyHash::operator()(const CounterKey& key) const noexcept
{
    size_t seed = 0;
    HashCombine(seed, key.target.groupId);
    HashCombine(seed, static_cast<uint32_t>(key.target.role));
    HashCombine(seed, key.flagId);
    return seed;
}

bool Synccheck::SetChannelKey::operator==(const SetChannelKey& other) const noexcept
{
    return mode == other.mode && flagId == other.flagId && sourceFlagId == other.sourceFlagId &&
           groupId == other.groupId && direction == other.direction && target == other.target;
}

size_t Synccheck::SetChannelKeyHash::operator()(const SetChannelKey& key) const noexcept
{
    size_t seed = 0;
    HashCombine(seed, key.mode);
    HashCombine(seed, key.flagId);
    HashCombine(seed, key.sourceFlagId);
    HashCombine(seed, key.groupId);
    HashCombine(seed, static_cast<uint32_t>(key.direction));
    HashCombine(seed, key.target.groupId);
    HashCombine(seed, static_cast<uint32_t>(key.target.role));
    return seed;
}

bool Synccheck::FlagUsageKey::operator==(const FlagUsageKey& other) const noexcept
{
    return participant == other.participant && flagId == other.flagId;
}

size_t Synccheck::FlagUsageKeyHash::operator()(const FlagUsageKey& key) const noexcept
{
    size_t seed = 0;
    HashCombine(seed, key.participant.groupId);
    HashCombine(seed, static_cast<uint32_t>(key.participant.role));
    HashCombine(seed, key.flagId);
    return seed;
}

LaunchTopology Synccheck::NormalizeTopology(const LaunchTopology& input)
{
    LaunchTopology topology = input;
    topology.mixType = KernelMixType::UNKNOWN;
    topology.aicPerGroup = 0;
    topology.aivPerGroup = 0;
    const auto kernelType = static_cast<AclKernelType>(topology.kernelType);
    if (topology.kernelTypeValid && kernelType == AclKernelType::CUBE) {
        topology.mixType = KernelMixType::AIC_ONLY;
        topology.aicPerGroup = 1;
    } else if (topology.kernelTypeValid && kernelType == AclKernelType::VECTOR) {
        topology.mixType = KernelMixType::AIV_ONLY;
        topology.aivPerGroup = 1;
    } else if (topology.kernelTypeValid && kernelType == AclKernelType::MIX && topology.ratioValid) {
        if (topology.aicRatio == 1 && topology.aivRatio == 1) {
            topology.mixType = KernelMixType::MIX_1_1;
            topology.aicPerGroup = 1;
            topology.aivPerGroup = 1;
        } else if (topology.aicRatio == 1 && topology.aivRatio == 2) {
            topology.mixType = KernelMixType::MIX_1_2;
            topology.aicPerGroup = 1;
            topology.aivPerGroup = 2;
        } else if (topology.aicRatio == 1 && topology.aivRatio == 0) {
            topology.mixType = KernelMixType::AIC_ONLY;
            topology.aicPerGroup = 1;
        } else if (topology.aicRatio == 0 && topology.aivRatio == 1) {
            topology.mixType = KernelMixType::AIV_ONLY;
            topology.aivPerGroup = 1;
        }
    }
    topology.expectedAicCount = topology.groupCountValid ? topology.groupCount * topology.aicPerGroup : 0;
    topology.expectedAivCount = topology.groupCountValid ? topology.groupCount * topology.aivPerGroup : 0;
    return topology;
}

void Synccheck::OnKernelLaunch(uint64_t launchId, const LaunchTopology& topology, void* stream)
{
    finalizedLaunches_.erase(launchId);
    LaunchSyncState state;
    state.launchId = launchId;
    state.stream = stream;
    state.topology = NormalizeTopology(topology);
    launchStates_.insert_or_assign(launchId, std::move(state));
}

Synccheck::SyncBufOccupancyKey Synccheck::BuildSyncBufOccupancyKey(const AclsanDeviceSyncData& data)
{
    return {data.objectId, data.header.phyCoreId};
}

bool Synccheck::ResolveParticipant(
    const LaunchTopology& topology, const AclsanDeviceSyncData& data, ParticipantKey& participant, std::string& error)
{
    if (!topology.groupCountValid || topology.mixType == KernelMixType::UNKNOWN) {
        error = "cross-core topology is incomplete";
        return false;
    }
    if (data.header.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE) {
        if (data.header.blockId >= topology.groupCount || topology.aicPerGroup == 0) {
            error = "AIC blockId is outside launch topology";
            return false;
        }
        participant = {data.header.blockId, ParticipantRole::AIC};
        return true;
    }
    if (data.header.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR || topology.aivPerGroup == 0 ||
        data.header.blockId >= topology.expectedAivCount) {
        error = "AIV blockId or blockType is outside launch topology";
        return false;
    }
    participant.groupId = data.header.blockId / topology.aivPerGroup;
    participant.role =
        topology.aivPerGroup == 2 && data.header.blockId % 2 != 0 ? ParticipantRole::AIV1 : ParticipantRole::AIV0;
    return true;
}

Synccheck::CanonicalCrossFlag Synccheck::CanonicalizeMode4SetFlag(const ParticipantKey& participant, uint64_t rawFlagId)
{
    if (participant.role == ParticipantRole::AIC) {
        const uint32_t sourceFlagId = static_cast<uint32_t>(rawFlagId & 0x1fU);
        return {sourceFlagId, sourceFlagId & 0xfU, sourceFlagId >> 4U};
    }
    const uint32_t sourceFlagId = static_cast<uint32_t>(rawFlagId & 0xfU);
    const uint32_t aivIndex = participant.role == ParticipantRole::AIV1 ? 1U : 0U;
    return {sourceFlagId, sourceFlagId + 16U * aivIndex, aivIndex};
}

uint32_t Synccheck::CanonicalizeLocalWaitFlag(const ParticipantKey& participant, uint32_t syncKind, uint64_t rawFlagId)
{
    if (syncKind == ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK && participant.role == ParticipantRole::AIC) {
        return static_cast<uint32_t>(rawFlagId & 0x1fU);
    }
    return static_cast<uint32_t>(rawFlagId & 0xfU);
}

bool Synccheck::ValidateCrossInstruction(const AclsanDeviceSyncData& data, std::string& error)
{
    const bool isWait = data.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT;
    const bool validCrossCoreSet =
        data.syncKind == ACLSAN_DEVICE_SYNC_KIND_CROSS_CORE && data.action == ACLSAN_DEVICE_SYNC_ACTION_SET &&
        (data.mode == ACLSAN_DEVICE_SYNC_MODE_GLOBAL || data.mode == ACLSAN_DEVICE_SYNC_MODE_GROUP_AIV ||
         data.mode == ACLSAN_DEVICE_SYNC_MODE_GROUP_AIC_AIV);
    const bool validIntraBlockSet = data.syncKind == ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK &&
                                    data.action == ACLSAN_DEVICE_SYNC_ACTION_SET &&
                                    data.mode == ACLSAN_DEVICE_SYNC_MODE_SINGLE_AIV;
    const bool valid = (IsCrossCoreSyncKind(data.syncKind) && isWait) || validCrossCoreSet || validIntraBlockSet;
    if (!valid) {
        error = "cross-core instruction/action/mode combination is inconsistent";
    }
    return valid;
}

bool Synccheck::BuildCounterContributions(
    const LaunchSyncState& state, const ParticipantKey& participant, uint32_t mode, uint32_t sourceFlagId,
    uint32_t targetFlagId, uint32_t aivIndex, std::vector<CounterContribution>& contributions, std::string& error)
{
    const LaunchTopology& topology = state.topology;
    if (!topology.groupCountValid || topology.mixType == KernelMixType::UNKNOWN) {
        error = "cross-core topology is incomplete";
        return false;
    }
    const auto addGroupAivs = [&topology](ParticipantSet& set, uint32_t group) {
        set.insert({group, ParticipantRole::AIV0});
        if (topology.aivPerGroup == 2) {
            set.insert({group, ParticipantRole::AIV1});
        }
    };
    const auto append = [&contributions, sourceFlagId, targetFlagId, mode](
                            const ParticipantKey& target, const ParticipantSet& required, uint32_t groupId,
                            CrossDirection direction) {
        CounterContribution contribution;
        contribution.targetCounter = {target, targetFlagId};
        contribution.requiredSetParticipants = required;
        contribution.sourceFlagId = sourceFlagId;
        contribution.mode = mode;
        contribution.groupId = groupId;
        contribution.direction = direction;
        contributions.push_back(std::move(contribution));
    };
    const uint32_t groupId = participant.groupId;
    if (mode == 0) {
        ParticipantSet required;
        if (participant.role == ParticipantRole::AIC && topology.aicPerGroup != 0) {
            for (uint32_t group = 0; group < topology.groupCount; ++group) {
                required.insert({group, ParticipantRole::AIC});
            }
            for (uint32_t group = 0; group < topology.groupCount; ++group) {
                append({group, ParticipantRole::AIC}, required, kGlobalGroup, CrossDirection::ALL_AIC);
            }
        } else if (participant.role != ParticipantRole::AIC && topology.aivPerGroup != 0) {
            for (uint32_t group = 0; group < topology.groupCount; ++group) {
                addGroupAivs(required, group);
            }
            for (const ParticipantKey& target : required) {
                append(target, required, kGlobalGroup, CrossDirection::ALL_AIV);
            }
        } else {
            error = "mode0 participant class is incompatible with topology";
            return false;
        }
    } else if (groupId >= topology.groupCount) {
        error = "cross-core groupId is outside launch topology";
        return false;
    } else if (
        mode == 1 && (topology.mixType == KernelMixType::MIX_1_1 || topology.mixType == KernelMixType::MIX_1_2) &&
        participant.role != ParticipantRole::AIC) {
        ParticipantSet required;
        addGroupAivs(required, groupId);
        for (const ParticipantKey& target : required) {
            append(target, required, groupId, CrossDirection::AIV_TO_AIV);
        }
    } else if (
        mode == 2 && (topology.mixType == KernelMixType::MIX_1_1 || topology.mixType == KernelMixType::MIX_1_2)) {
        ParticipantSet required;
        if (participant.role == ParticipantRole::AIC) {
            required.insert(participant);
            ParticipantSet targets;
            addGroupAivs(targets, groupId);
            for (const ParticipantKey& target : targets) {
                append(target, required, groupId, CrossDirection::AIC_TO_AIV);
            }
        } else {
            addGroupAivs(required, groupId);
            append({groupId, ParticipantRole::AIC}, required, groupId, CrossDirection::AIV_TO_AIC);
        }
    } else if (mode == 4 && topology.mixType == KernelMixType::MIX_1_2 && aivIndex < 2U) {
        const ParticipantKey aiv{groupId, aivIndex == 0 ? ParticipantRole::AIV0 : ParticipantRole::AIV1};
        const ParticipantKey aic{groupId, ParticipantRole::AIC};
        ParticipantSet required;
        if (participant.role == ParticipantRole::AIC) {
            required.insert(aic);
            append(aiv, required, groupId, CrossDirection::AIC_TO_AIV);
        } else if (participant == aiv) {
            required.insert(aiv);
            append(aic, required, groupId, CrossDirection::AIV_TO_AIC);
        } else {
            error = "mode4 flag selects a different AIV participant";
            return false;
        }
    } else if (mode == 1) {
        std::ostringstream reason;
        reason << "cross-core mode1 only synchronizes AIV participants within a MIX1:1 or MIX1:2 group; observed "
               << "topology=" << KernelMixTypeName(topology.mixType)
               << " and source=" << ParticipantSetText(ParticipantSet{participant});
        error = reason.str();
        return false;
    } else if (mode == 2) {
        std::ostringstream reason;
        reason << "cross-core mode2 requires MIX1:1 or MIX1:2 topology for AIC-to-all-AIV synchronization; observed "
               << "topology=" << KernelMixTypeName(topology.mixType);
        error = reason.str();
        return false;
    } else if (mode == 4) {
        std::ostringstream reason;
        reason << "cross-core mode4 only supports MIX1:2 topology (__mix__(1, 2) / KERNEL_TYPE_MIX_AIC_1_2) "
               << "for synchronization between one AIC and one selected AIV; observed topology="
               << KernelMixTypeName(topology.mixType) << ". On the AIC side flagId 0-15 selects AIV0 and 16-31 "
               << "selects AIV1; on the AIV side flagId must be 0-15";
        error = reason.str();
        return false;
    } else {
        std::ostringstream reason;
        reason << "unsupported cross-core mode=" << mode << "; supported mode values are 0, 1, 2, and 4";
        error = reason.str();
        return false;
    }
    return !contributions.empty();
}

const char* Synccheck::DirectionName(CrossDirection direction)
{
    switch (direction) {
        case CrossDirection::ALL_AIC:
            return "ALL_AIC";
        case CrossDirection::ALL_AIV:
            return "ALL_AIV";
        case CrossDirection::AIV_TO_AIV:
            return "AIV_TO_AIV";
        case CrossDirection::AIC_TO_AIV:
            return "AIC_TO_AIV";
        case CrossDirection::AIV_TO_AIC:
            return "AIV_TO_AIC";
        default:
            return "UNKNOWN";
    }
}

std::string Synccheck::ParticipantSetText(const ParticipantSet& participants)
{
    std::vector<std::pair<uint32_t, ParticipantRole>> sorted;
    for (const auto& participant : participants) {
        sorted.emplace_back(participant.groupId, participant.role);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return std::tie(left.first, left.second) < std::tie(right.first, right.second);
    });
    std::ostringstream output;
    output << '{';
    for (size_t index = 0; index < sorted.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const char* role = sorted[index].second == ParticipantRole::AIC  ? "AIC" :
                           sorted[index].second == ParticipantRole::AIV0 ? "AIV0" :
                           sorted[index].second == ParticipantRole::AIV1 ? "AIV1" :
                                                                           "UNKNOWN";
        output << "group" << sorted[index].first << '/' << role;
    }
    output << '}';
    return output.str();
}

void Synccheck::HandleFlagEvent(
    FlagState& state, const AclsanDeviceSyncData& data, const FlagPairKey& key, Reports& reports)
{
    if (data.action == ACLSAN_DEVICE_SYNC_ACTION_SET) {
        const auto [open, inserted] = state.pending.emplace(key, data);
        if (!inserted) {
            ++stats_.duplicateOpens;
            reports.push_back(BuildMismatch(
                data, &open->second, static_cast<uint32_t>(NpuCheckSyncMismatchReason::DUPLICATE_OPEN), key));
        } else {
            ++stats_.pendingOpens;
        }
        return;
    }
    const auto open = state.pending.find(key);
    if (data.action != ACLSAN_DEVICE_SYNC_ACTION_WAIT || open == state.pending.end()) {
        ++stats_.unmatchedCloses;
        reports.push_back(
            BuildMismatch(data, nullptr, static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE), key));
    } else {
        state.pending.erase(open);
        ++stats_.matchedPairs;
        --stats_.pendingOpens;
    }
}

void Synccheck::HandleSyncBufEvent(
    SyncBufState& state, const AclsanDeviceSyncData& data, const SyncBufPairKey& key, Reports& reports)
{
    if (data.action == ACLSAN_DEVICE_SYNC_ACTION_GET) {
        const SyncBufOccupancyKey occupancyKey = BuildSyncBufOccupancyKey(data);
        const auto active = state.activeSyncBufs.find(occupancyKey);
        if (active != state.activeSyncBufs.end()) {
            ++stats_.duplicateOpens;
            const auto& open = state.pending.at(active->second);
            reports.push_back(
                BuildMismatch(data, &open, static_cast<uint32_t>(NpuCheckSyncMismatchReason::DUPLICATE_OPEN), key));
        } else {
            state.pending.emplace(key, data);
            state.activeSyncBufs.emplace(occupancyKey, key);
            ++stats_.pendingOpens;
        }
        return;
    }
    const auto open = state.pending.find(key);
    if (data.action != ACLSAN_DEVICE_SYNC_ACTION_RELEASE || open == state.pending.end()) {
        ++stats_.unmatchedCloses;
        reports.push_back(
            BuildMismatch(data, nullptr, static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE), key));
    } else {
        state.activeSyncBufs.erase(BuildSyncBufOccupancyKey(data));
        state.pending.erase(open);
        ++stats_.matchedPairs;
        --stats_.pendingOpens;
    }
}

bool Synccheck::ValidateFlagMode(
    LaunchSyncState& state, const FlagUsageKey& key, uint32_t mode, const AclsanDeviceSyncData& trigger)
{
    if (mode == ACLSAN_DEVICE_SYNC_MODE_UNKNOWN) {
        return true;
    }
    CleanupFlagUsage(state, key);
    const auto usage = state.crossCore.flagUsage.find(key);
    if (usage == state.crossCore.flagUsage.end() || usage->second.activeMode == mode) {
        return true;
    }
    ++stats_.sequenceMismatches;
    std::ostringstream reason;
    const CounterKey counterKey{key.participant, key.flagId};
    const auto counter = state.crossCore.counters.find(counterKey);
    const uint32_t counterValue = counter == state.crossCore.counters.end() ? 0U : counter->second.value;
    reason << "FLAG_MODE_SWITCH_BEFORE_COMPLETION participant=" << ParticipantSetText(ParticipantSet{key.participant})
           << " flagId=" << key.flagId << " activeMode=" << usage->second.activeMode << " newMode=" << mode
           << " pendingSetRounds=" << usage->second.pendingSetRoundRefs << " remainingCount=" << counterValue
           << "; complete all SET aggregation and consume the remaining counter count before changing mode";
    state.reports.push_back(BuildSequence(trigger, reason.str()));
    return false;
}

void Synccheck::CleanupFlagUsage(LaunchSyncState& state, const FlagUsageKey& key)
{
    const auto usage = state.crossCore.flagUsage.find(key);
    if (usage == state.crossCore.flagUsage.end() || usage->second.pendingSetRoundRefs != 0) {
        return;
    }
    const CounterKey counterKey{key.participant, key.flagId};
    const auto counter = state.crossCore.counters.find(counterKey);
    if (counter == state.crossCore.counters.end() || counter->second.value == 0) {
        state.crossCore.flagUsage.erase(usage);
    }
}

void Synccheck::ActivateSetRoundUsage(LaunchSyncState& state, const SetChannelKey& key, const SetRound& round)
{
    std::unordered_set<FlagUsageKey, FlagUsageKeyHash> usageKeys;
    for (const ParticipantKey& participant : round.expectedSet) {
        usageKeys.insert({participant, key.sourceFlagId});
    }
    usageKeys.insert({key.target, key.flagId});
    for (const FlagUsageKey& usageKey : usageKeys) {
        FlagUsageState& usage = state.crossCore.flagUsage[usageKey];
        usage.activeMode = key.mode;
        ++usage.pendingSetRoundRefs;
    }
}

void Synccheck::ReleaseSetRoundUsage(LaunchSyncState& state, const SetChannelKey& key, const SetRound& round)
{
    std::unordered_set<FlagUsageKey, FlagUsageKeyHash> usageKeys;
    for (const ParticipantKey& participant : round.expectedSet) {
        usageKeys.insert({participant, key.sourceFlagId});
    }
    usageKeys.insert({key.target, key.flagId});
    for (const FlagUsageKey& usageKey : usageKeys) {
        auto usage = state.crossCore.flagUsage.find(usageKey);
        if (usage != state.crossCore.flagUsage.end() && usage->second.pendingSetRoundRefs != 0) {
            --usage->second.pendingSetRoundRefs;
        }
        CleanupFlagUsage(state, usageKey);
    }
}

void Synccheck::ProduceCounterToken(
    LaunchSyncState& state, const CounterContribution& contribution, const AclsanDeviceSyncData& trigger)
{
    CounterState& counter = state.crossCore.counters[contribution.targetCounter];
    const bool mode4Contribution = contribution.mode == ACLSAN_DEVICE_SYNC_MODE_SINGLE_AIV;
    const auto compatibleWait = std::find_if(
        counter.pendingWaits.begin(), counter.pendingWaits.end(),
        [mode4Contribution](const AclsanDeviceSyncData& wait) {
            return IsMode4WaitInstruction(wait) == mode4Contribution;
        });
    ++stats_.producedCrossTokens;
    if (compatibleWait != counter.pendingWaits.end()) {
        counter.pendingWaits.erase(compatibleWait);
        ++stats_.consumedCrossTokens;
        return;
    }
    counter.lastProducer = trigger;
    if (counter.value == 15U) {
        ++stats_.counterOverflows;
        ++stats_.sequenceMismatches;
        std::ostringstream reason;
        reason << "COUNTER_OVERFLOW participant="
               << ParticipantSetText(ParticipantSet{contribution.targetCounter.target})
               << " flagId=" << contribution.targetCounter.flagId << " mode=" << contribution.mode
               << " currentCount=15 attemptedCount=16 maximum=15; the completed SET aggregation cannot increment "
                  "this counter until CrossCoreWaitFlag consumes an existing count";
        state.reports.push_back(BuildSequence(trigger, reason.str()));
        return;
    }
    ++counter.value;
    const FlagUsageKey usageKey{contribution.targetCounter.target, contribution.targetCounter.flagId};
    FlagUsageState& usage = state.crossCore.flagUsage[usageKey];
    usage.activeMode = contribution.mode;
}

void Synccheck::ProcessCrossSet(
    LaunchSyncState& state, const ParticipantKey& participant, const AclsanDeviceSyncData& data,
    const CanonicalCrossFlag& flag)
{
    std::vector<CounterContribution> contributions;
    std::string error;
    if (!BuildCounterContributions(
            state, participant, data.mode, flag.sourceFlagId, flag.targetFlagId, flag.aivIndex, contributions, error)) {
        ++stats_.invalidArguments;
        state.reports.push_back(BuildInvalid(data, error));
        return;
    }

    std::unordered_set<FlagUsageKey, FlagUsageKeyHash> affectedUsage;
    for (const CounterContribution& contribution : contributions) {
        affectedUsage.insert({contribution.targetCounter.target, contribution.targetCounter.flagId});
        for (const ParticipantKey& source : contribution.requiredSetParticipants) {
            affectedUsage.insert({source, contribution.sourceFlagId});
        }
    }
    for (const FlagUsageKey& usageKey : affectedUsage) {
        if (!ValidateFlagMode(state, usageKey, data.mode, data)) {
            return;
        }
    }

    for (const CounterContribution& contribution : contributions) {
        const SetChannelKey key{contribution.mode,         contribution.targetCounter.flagId,
                                contribution.sourceFlagId, contribution.groupId,
                                contribution.direction,    contribution.targetCounter.target};
        SetChannelState& channel = state.crossCore.pendingSetRounds[key];
        const uint64_t roundId = channel.setOccurrences[participant]++;
        auto [roundIt, inserted] = channel.pendingRounds.try_emplace(roundId);
        SetRound& round = roundIt->second;
        if (inserted) {
            round.expectedSet = contribution.requiredSetParticipants;
            round.firstEvent = data;
            ActivateSetRoundUsage(state, key, round);
        }
        round.actualSet.insert(participant);
        if (!SetEquals(round.expectedSet, round.actualSet)) {
            continue;
        }
        SetRound completed = std::move(round);
        channel.pendingRounds.erase(roundIt);
        ReleaseSetRoundUsage(state, key, completed);
        ProduceCounterToken(state, contribution, data);
        ++stats_.matchedCrossRounds;
    }
}

void Synccheck::ProcessCrossWait(
    LaunchSyncState& state, const ParticipantKey& participant, const AclsanDeviceSyncData& data, uint32_t flagId)
{
    const FlagUsageKey usageKey{participant, flagId};
    const CounterKey key{participant, flagId};
    CounterState& counter = state.crossCore.counters[key];
    const bool mode4Counter = counter.lastProducer.mode == ACLSAN_DEVICE_SYNC_MODE_SINGLE_AIV;
    if (counter.value != 0 && IsMode4WaitInstruction(data) == mode4Counter) {
        --counter.value;
        ++stats_.consumedCrossTokens;
        CleanupFlagUsage(state, usageKey);
        return;
    }

    // Device trace records are drained per physical-core slice, so a peer SET may be delivered later.
    // This queue is only a deferred debit on this local counter; it is not a WAIT participant round.
    counter.pendingWaits.push_back(data);
}

void Synccheck::HandleCrossCoreEvent(LaunchSyncState& state, const AclsanDeviceSyncData& data)
{
    ParticipantKey participant;
    std::string error;
    if (!ResolveParticipant(state.topology, data, participant, error)) {
        ++stats_.invalidArguments;
        state.reports.push_back(BuildInvalid(data, error));
        return;
    }
    if (!ValidateCrossInstruction(data, error)) {
        ++stats_.invalidArguments;
        state.reports.push_back(BuildInvalid(data, error));
        return;
    }
    if (data.action == ACLSAN_DEVICE_SYNC_ACTION_SET) {
        const CanonicalCrossFlag flag = data.mode == ACLSAN_DEVICE_SYNC_MODE_SINGLE_AIV ?
                                            CanonicalizeMode4SetFlag(participant, data.objectId) :
                                            CanonicalCrossFlag{
                                                static_cast<uint32_t>(data.objectId & 0xfU),
                                                static_cast<uint32_t>(data.objectId & 0xfU), kInvalidAivIndex};
        ProcessCrossSet(state, participant, data, flag);
    } else {
        ProcessCrossWait(
            state, participant, data, CanonicalizeLocalWaitFlag(participant, data.syncKind, data.objectId));
    }
}

void Synccheck::ProcessSyncEvent(LaunchSyncState& state, const AclsanDeviceSyncData& data)
{
    switch (data.syncKind) {
        case ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG:
            HandleFlagEvent(state.flags, data, FlagPairKey::FromCbdata(data), state.reports);
            break;
        case ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF:
            HandleSyncBufEvent(state.syncBufs, data, SyncBufPairKey::FromCbdata(data), state.reports);
            break;
        case ACLSAN_DEVICE_SYNC_KIND_CROSS_CORE:
        case ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK:
            HandleCrossCoreEvent(state, data);
            break;
        default:
            ++stats_.invalidArguments;
            state.reports.push_back(BuildInvalid(data, "unknown synchronization kind"));
            break;
    }
}

void Synccheck::OnDeviceSync(const AclsanDeviceSyncData& data)
{
    ++stats_.syncEvents;
    const auto state = launchStates_.find(data.header.launchId);
    if (state == launchStates_.end()) {
        ++stats_.invalidArguments;
        orphanReports_.push_back(BuildInvalid(
            data, finalizedLaunches_.count(data.header.launchId) != 0 ?
                      "DEVICE_SYNC arrived after launch finalization" :
                      "DEVICE_SYNC arrived before launch callback"));
        return;
    }
    ProcessSyncEvent(state->second, data);
}

void Synccheck::FinalizeCrossCore(LaunchSyncState& state, Reports& reports)
{
    for (const auto& channelEntry : state.crossCore.pendingSetRounds) {
        for (const auto& roundEntry : channelEntry.second.pendingRounds) {
            ++stats_.participantMismatches;
            reports.push_back(BuildParticipantMismatch(
                channelEntry.first, roundEntry.first, roundEntry.second, roundEntry.second.firstEvent));
        }
    }
    for (const auto& counterEntry : state.crossCore.counters) {
        const CounterKey& key = counterEntry.first;
        const CounterState& counter = counterEntry.second;
        if (counter.value != 0) {
            stats_.unconsumedSyncTokens += counter.value;
            ++stats_.unconsumedSyncTokenReports;
            reports.push_back(BuildCrossTokenMismatch(
                key, counter, counter.lastProducer, static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNCONSUMED_OPEN),
                counter.value));
        }
        for (const AclsanDeviceSyncData& wait : counter.pendingWaits) {
            ++stats_.waitWithoutSyncTokens;
            reports.push_back(BuildCrossTokenMismatch(
                key, counter, wait, static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE), 0));
        }
    }
}

std::vector<Synccheck::Report> Synccheck::OnSynchronization() { return FinalizeLaunches(nullptr, false); }

std::vector<Synccheck::Report> Synccheck::OnSynchronization(void* stream) { return FinalizeLaunches(stream, true); }

std::vector<Synccheck::Report> Synccheck::FinalizeLaunches(void* stream, bool filterByStream)
{
    ++stats_.synchronizationEvents;
    Reports reports = std::move(orphanReports_);
    orphanReports_.clear();
    const auto reportPending = [this, &reports](const auto& pending) {
        for (const auto& entry : pending) {
            ++stats_.unconsumedOpens;
            reports.push_back(BuildMismatch(
                entry.second, nullptr, static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNCONSUMED_OPEN),
                entry.first));
        }
    };
    for (auto stateEntry = launchStates_.begin(); stateEntry != launchStates_.end();) {
        LaunchSyncState& state = stateEntry->second;
        if (filterByStream && state.stream != stream) {
            ++stateEntry;
            continue;
        }
        for (Report& report : state.reports) {
            reports.push_back(std::move(report));
        }
        reportPending(state.flags.pending);
        reportPending(state.syncBufs.pending);
        FinalizeCrossCore(state, reports);
        stats_.pendingOpens -= state.flags.pending.size() + state.syncBufs.pending.size();
        finalizedLaunches_.insert(state.launchId);
        stateEntry = launchStates_.erase(stateEntry);
    }
    return reports;
}

SynccheckStats Synccheck::Stats() const { return stats_; }

std::optional<LaunchTopology> Synccheck::TopologyForLaunch(uint64_t launchId) const
{
    const auto state = launchStates_.find(launchId);
    if (state == launchStates_.end()) {
        return std::nullopt;
    }
    return state->second.topology;
}

std::optional<uint32_t> Synccheck::CrossCounterValue(
    uint64_t launchId, uint32_t groupId, ParticipantRole role, uint32_t flagId) const
{
    const auto state = launchStates_.find(launchId);
    if (state == launchStates_.end()) {
        return std::nullopt;
    }
    const CounterKey key{{groupId, role}, flagId};
    const auto counter = state->second.crossCore.counters.find(key);
    return counter == state->second.crossCore.counters.end() ? std::optional<uint32_t>{0} :
                                                               std::optional<uint32_t>{counter->second.value};
}

size_t Synccheck::PendingCrossSetRoundCount(uint64_t launchId) const
{
    const auto state = launchStates_.find(launchId);
    if (state == launchStates_.end()) {
        return 0;
    }
    size_t count = 0;
    for (const auto& channel : state->second.crossCore.pendingSetRounds) {
        count += channel.second.pendingRounds.size();
    }
    return count;
}

NpuCheckSyncPoint Synccheck::ActualPoint(const AclsanDeviceSyncData& data)
{
    NpuCheckSyncPoint point{};
    point.operation = OperationName(data);
    point.hasExecContext = true;
    point.exec = ExecContext(data);
    return point;
}

NpuCheckSyncPoint Synccheck::ExpectedPoint(const AclsanDeviceSyncData& data, uint32_t reason)
{
    NpuCheckSyncPoint point{};
    point.operation = ExpectedOperation(data, reason);
    return point;
}

Synccheck::Report Synccheck::BuildReportBase(
    const AclsanDeviceSyncData& trigger, NpuCheckReportPattern pattern, NpuCheckSyncPrimitiveKind primitiveKind,
    NpuCheckSyncDetailKind detailKind)
{
    Report report{};
    report.common.tool = ReportTool::SYNCCHECK;
    report.common.severity = ReportSeverity::ERROR;
    report.common.pattern = pattern;
    report.common.flags = kNpuCheckReportCommonHasExecContext;
    report.primitiveKind = primitiveKind;
    report.detailKind = detailKind;
    report.triggerPoint = ActualPoint(trigger);
    report.common.exec = report.triggerPoint.exec;
    return report;
}

Synccheck::Report Synccheck::BuildMismatchBase(
    const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason)
{
    const NpuCheckSyncPrimitiveKind primitiveKind =
        trigger.syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG ? NpuCheckSyncPrimitiveKind::SET_WAIT_FLAG :
        IsCrossCoreSyncKind(trigger.syncKind)                     ? NpuCheckSyncPrimitiveKind::SYNC_OBJECT :
                                                                    NpuCheckSyncPrimitiveKind::GET_RLS_BUF;
    Report report = BuildReportBase(
        trigger, NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH, primitiveKind, NpuCheckSyncDetailKind::PAIRING);
    report.hasRelatedPoint = true;
    report.relatedPoint = related == nullptr ? ExpectedPoint(trigger, reason) : ActualPoint(*related);
    return report;
}

Synccheck::Report Synccheck::BuildMismatch(
    const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason, const FlagPairKey& key)
{
    Report report = BuildMismatchBase(trigger, related, reason);
    NpuCheckSyncPairingError detail{};
    detail.reason = static_cast<NpuCheckSyncMismatchReason>(reason);
    detail.key.pairKind = NpuCheckSyncPairKind::SET_WAIT_FLAG;
    detail.key.srcPipe = key.srcPipe;
    detail.key.dstPipe = key.dstPipe;
    detail.key.id = key.objectId;
    report.detail = detail;
    return report;
}

Synccheck::Report Synccheck::BuildMismatch(
    const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason,
    const SyncBufPairKey& key)
{
    Report report = BuildMismatchBase(trigger, related, reason);
    NpuCheckSyncPairingError detail{};
    detail.reason = static_cast<NpuCheckSyncMismatchReason>(reason);
    detail.key.pairKind = NpuCheckSyncPairKind::GET_RLS_BUF;
    detail.key.dstPipe = key.pipe;
    detail.key.mode = key.mode;
    detail.key.id = key.objectId;
    report.detail = detail;
    return report;
}

Synccheck::Report Synccheck::BuildInvalid(const AclsanDeviceSyncData& trigger, const std::string& reason)
{
    const NpuCheckSyncPrimitiveKind primitiveKind = IsCrossCoreSyncKind(trigger.syncKind) ?
                                                        NpuCheckSyncPrimitiveKind::BARRIER :
                                                        NpuCheckSyncPrimitiveKind::SYNC_OBJECT;
    Report report = BuildReportBase(
        trigger, NpuCheckReportPattern::SYNCCHECK_INVALID_ARGUMENT, primitiveKind, NpuCheckSyncDetailKind::OBJECT);
    NpuCheckSyncObjectError detail{};
    detail.reason = reason;
    detail.objectId = trigger.objectId;
    report.detail = std::move(detail);
    return report;
}

Synccheck::Report Synccheck::BuildSequence(const AclsanDeviceSyncData& trigger, const std::string& reason)
{
    Report report = BuildReportBase(
        trigger, NpuCheckReportPattern::SYNCCHECK_INSTRUCTION_SEQUENCE_MISMATCH,
        NpuCheckSyncPrimitiveKind::INSTRUCTION_SEQUENCE, NpuCheckSyncDetailKind::SEQUENCE);
    report.hasRelatedPoint = true;
    report.relatedPoint.operation = "CROSS_CORE_PEER";
    NpuCheckSyncSequenceError detail{};
    detail.reason = reason;
    report.detail = std::move(detail);
    return report;
}

Synccheck::Report Synccheck::BuildParticipantMismatch(
    const SetChannelKey& key, uint64_t roundId, const SetRound& round, const AclsanDeviceSyncData& trigger)
{
    ParticipantSet missingSet;
    for (const ParticipantKey& participant : round.expectedSet) {
        if (round.actualSet.count(participant) == 0)
            missingSet.insert(participant);
    }
    std::ostringstream reason;
    reason << "mode=" << key.mode << " flagId=" << key.flagId << " groupId=" << key.groupId << " roundId=" << roundId
           << " direction=" << DirectionName(key.direction)
           << " target=" << ParticipantSetText(ParticipantSet{key.target})
           << " expectedSet=" << ParticipantSetText(round.expectedSet)
           << " actualSet=" << ParticipantSetText(round.actualSet) << " missingSet=" << ParticipantSetText(missingSet);

    Report report = BuildReportBase(
        trigger, NpuCheckReportPattern::SYNCCHECK_PARTICIPANT_MISMATCH, NpuCheckSyncPrimitiveKind::BARRIER,
        NpuCheckSyncDetailKind::BARRIER);
    NpuCheckSyncBarrierError detail{};
    detail.reason = reason.str();
    detail.scope = DirectionName(key.direction);
    detail.objectId = key.flagId;
    report.detail = std::move(detail);
    return report;
}

Synccheck::Report Synccheck::BuildCrossTokenMismatch(
    const CounterKey& key, const CounterState& counter, const AclsanDeviceSyncData& trigger, uint32_t reason,
    uint32_t tokenCount)
{
    Report report = BuildMismatchBase(trigger, nullptr, reason);
    NpuCheckSyncPairingError detail{};
    detail.reason = static_cast<NpuCheckSyncMismatchReason>(reason);
    detail.key.pairKind = NpuCheckSyncPairKind::CROSS_CORE_TOKEN;
    detail.key.mode = reason == static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE) ?
                          trigger.mode :
                          counter.lastProducer.mode;
    detail.key.id = key.flagId;
    detail.key.groupId = key.target.groupId;
    detail.key.participantRole = static_cast<uint32_t>(key.target.role);
    detail.key.tokenCount = tokenCount;
    report.detail = detail;
    return report;
}

} // namespace npucheck
