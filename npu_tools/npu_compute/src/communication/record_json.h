/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_COMMUNICATION_RECORD_JSON_H
#define NPU_COMPUTE_COMMUNICATION_RECORD_JSON_H
#include "aclpti/aclpti_communication.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace npucompute::communication {
using Tree = boost::property_tree::ptree;
// Staging schema stores integer counters/IDs as decimal strings, avoiding JSON double truncation.
#define COMM_RECORD_FIELDS(F) \
    do {                      \
        F(deviceId);          \
        F(streamId);          \
        F(taskId);            \
        F(dieId);             \
        F(planeId);           \
        F(version);           \
        F(turn);              \
        F(totalTurns);        \
        F(instrId);           \
        F(dataLen);           \
        F(itemId);            \
        F(groupName);         \
        F(rankId);            \
        F(remoteRank);        \
        F(rankSize);          \
        F(missionId);         \
        F(workFlowMode);      \
        F(setckeInstrId);     \
        F(ckeId);             \
        F(mask);              \
        F(opType);            \
        F(dataType);          \
        F(outputDataType);    \
        F(linkType);          \
        F(transportType);     \
        F(rdmaType);          \
        F(role);              \
        F(stage);             \
        F(ctxId);             \
        F(timestamp);         \
        F(cclTag);            \
        F(notifyId);          \
        F(srcAddr);           \
        F(dstAddr);           \
        F(threadId);          \
        F(payloadDeviceId);   \
        F(rawType);           \
        F(level);             \
        F(type);              \
        F(kernelType);        \
        F(dispatchTime);      \
        F(totalTime);         \
        F(fftsThreadId);      \
    } while (0)
template <class T, size_t N>
Tree Array(const std::array<T, N>& values)
{
    Tree result;
    for (const auto value : values) {
        Tree item;
        item.put_value(+value);
        result.push_back({"", item});
    }
    return result;
}
template <class T, size_t N>
void ReadArray(const Tree& tree, std::array<T, N>& values)
{
    if (tree.size() != N)
        throw std::runtime_error("invalid Communication array length");
    size_t i = 0;
    for (const auto& item : tree) {
        const auto value = item.second.get_value<decltype(+T{})>();
        if (value > std::numeric_limits<T>::max() || value < std::numeric_limits<T>::lowest())
            throw std::runtime_error("Communication array value out of range");
        values[i++] = static_cast<T>(value);
    }
}
inline Tree RecordTree(const aclpti::communication::Record& record)
{
    Tree tree;
    tree.put("kind", static_cast<unsigned>(record.kind));
#define COMM_PUT(name) tree.put(#name, record.name)
    COMM_RECORD_FIELDS(COMM_PUT);
#undef COMM_PUT
    tree.put("durationEstimated", record.durationEstimated);
    if (record.reserve)
        tree.put("reserve", *record.reserve);
    if (record.reserves)
        tree.add_child("reserves", Array(*record.reserves));
    if (record.kind == aclpti::communication::RecordKind::CcuChannel) {
        tree.put("channelId", record.channelId);
        tree.put("channelAverage", record.channelAverage);
    }
    if (record.kind == aclpti::communication::RecordKind::BlockStart ||
        record.kind == aclpti::communication::RecordKind::BlockEnd ||
        record.kind == aclpti::communication::RecordKind::AicoreTimestamp) {
        tree.put("blockId", record.blockId);
        tree.put("subBlockId", record.subBlockId);
        tree.put("coreType", record.coreType);
        tree.put("coreId", record.coreId);
        tree.put("descId", record.descId);
        tree.put("curPc", record.curPc);
    }
    tree.put("taskType", record.taskType);
    tree.put("functionType", record.functionType);
    tree.put("fusionType", record.fusionType);
    tree.add_child("times", Array(record.times));
    tree.add_child("relative", Array(record.relative));
    tree.add_child("channelIds", Array(record.channelIds));
    tree.add_child("remoteRanks", Array(record.remoteRanks));
    tree.add_child("nodeTicks", Array(record.nodeTicks));
    return tree;
}
inline aclpti::communication::Record ReadRecord(const Tree& tree)
{
    aclpti::communication::Record record;
    const auto kind = tree.get<unsigned>("kind");
    if (kind > static_cast<unsigned>(aclpti::communication::RecordKind::AicoreTimestamp))
        throw std::runtime_error("invalid Communication record kind");
    record.kind = static_cast<aclpti::communication::RecordKind>(kind);
#define COMM_GET(name) record.name = tree.get<decltype(record.name)>(#name)
    COMM_RECORD_FIELDS(COMM_GET);
#undef COMM_GET
    record.durationEstimated = tree.get<double>("durationEstimated");
    if (const auto reserve = tree.get_optional<uint32_t>("reserve"))
        record.reserve = *reserve;
    if (const auto reserves = tree.get_child_optional("reserves"))
        ReadArray(*reserves, record.reserves.emplace());
    if (record.kind == aclpti::communication::RecordKind::CcuChannel) {
        record.channelId = tree.get<uint16_t>("channelId");
        record.channelAverage = tree.get<uint32_t>("channelAverage");
    }
    if (record.kind == aclpti::communication::RecordKind::BlockStart ||
        record.kind == aclpti::communication::RecordKind::BlockEnd ||
        record.kind == aclpti::communication::RecordKind::AicoreTimestamp) {
        record.blockId = tree.get<uint16_t>("blockId");
        record.subBlockId = tree.get<uint16_t>("subBlockId");
        record.coreType = tree.get<uint32_t>("coreType");
        record.coreId = tree.get<uint16_t>("coreId");
        record.descId = tree.get<uint32_t>("descId");
        record.curPc = tree.get<uint64_t>("curPc");
    }
    record.taskType = tree.get<uint32_t>("taskType", UINT32_MAX);
    record.functionType = tree.get<uint32_t>("functionType", UINT32_MAX);
    record.fusionType = tree.get<uint32_t>("fusionType", 0);
    ReadArray(tree.get_child("times"), record.times);
    ReadArray(tree.get_child("relative"), record.relative);
    ReadArray(tree.get_child("channelIds"), record.channelIds);
    ReadArray(tree.get_child("remoteRanks"), record.remoteRanks);
    ReadArray(tree.get_child("nodeTicks"), record.nodeTicks);
    return record;
}
#undef COMM_RECORD_FIELDS
inline Tree ClockTree(const aclpti::communication::ClockMapping& clock)
{
    Tree tree;
    tree.put("counterAnchor", clock.counterAnchor);
    tree.put("hostNsAnchor", clock.hostNsAnchor);
    tree.put("frequencyHz", clock.frequencyHz);
    tree.put("calibrated", clock.calibrated);
    return tree;
}
inline aclpti::communication::ClockMapping ReadClock(const Tree& tree)
{
    return {
        tree.get<uint64_t>("counterAnchor"), tree.get<uint64_t>("hostNsAnchor"), tree.get<uint64_t>("frequencyHz"),
        tree.get<bool>("calibrated")};
}
inline std::string Json(const Tree& tree)
{
    std::ostringstream output;
    boost::property_tree::write_json(output, tree, false);
    auto json = output.str();
    if (json.empty() || json.back() != '\n')
        json.push_back('\n');
    return json;
}
} // namespace npucompute::communication
#endif
