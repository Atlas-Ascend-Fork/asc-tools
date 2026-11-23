/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "acl_pti/data/communication_decoder.h"
#include "securec.h"
#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace aclpti::communication;
void Put(std::vector<uint8_t>& b, size_t p, uint64_t v, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        b[p + i] = static_cast<uint8_t>(v >> (8 * i));
}
MsprofRawData Chunk(const std::vector<uint8_t>& b, int type, size_t begin, size_t n, size_t offset, bool last)
{
    MsprofRawData r{};
    r.deviceId = 7;
    r.chunkModule = 3;
    const int32_t wireType = type;
    static_assert(sizeof(r.type) == sizeof(wireType));
    if (memcpy_s(&r.type, sizeof(r.type), &wireType, sizeof(wireType)) != EOK) {
        std::abort();
    }
    r.offset = offset;
    r.chunkSize = n;
    r.isLastChunk = last;
    if (begin > b.size() || n > b.size() - begin ||
        (n != 0 && memcpy_s(r.chunk, sizeof(r.chunk), b.data() + begin, n) != EOK)) {
        std::abort();
    }
    return r;
}
std::vector<uint8_t> Additional(uint32_t type, uint32_t len, uint16_t level = 6000)
{
    std::vector<uint8_t> b(256);
    Put(b, 0, 0x5a5a, 2);
    Put(b, 2, level, 2);
    Put(b, 4, type, 4);
    Put(b, 12, len, 4);
    return b;
}
void AdditionalWhitelist()
{
    std::vector<Record> out;
    Decoder decoder;
    // Type17 carries arbitrary-length dictionaries as well as binary records.
    const std::string text = "20500_0:msproftx\n" + std::string(580, 'x');
    std::vector<uint8_t> dictionary(text.begin(), text.end());
    assert(decoder.Feed(Chunk(dictionary, 17, 0, 1, 0, false), out));
    assert(decoder.Feed(Chunk(dictionary, 17, 1, 2, 1, false), out));
    for (size_t at = 3; at < dictionary.size();) {
        const auto n = std::min(size_t{256}, dictionary.size() - at);
        assert(decoder.Feed(Chunk(dictionary, 17, at, n, at, at + n == dictionary.size()), out));
        at += n;
    }
    assert(out.empty());
    const std::vector<uint8_t> shortUnrelated{0x5a, 0x5a};
    assert(decoder.Feed(Chunk(shortUnrelated, 17, 0, 2, 0, true), out));
    assert(out.empty());

    // Unrelated records are skipped without interpreting their payload length.
    auto unrelated = Additional(12, UINT32_MAX, 10000);
    auto wrongChannel = Additional(5, 48, 6000);
    assert(decoder.Feed(Chunk(unrelated, 17, 0, 256, 0, true), out));
    assert(decoder.Feed(Chunk(wrongChannel, 17, 0, 256, 0, true), out));
    assert(out.empty());

    // An unrelated binary record must not hide a supported record in the same package.
    auto task = Additional(14, 48, 5500);
    auto group = Additional(16, 152, 5500);
    auto wait = Additional(15, 152, 5500);
    std::vector<uint8_t> mixed = unrelated;
    for (const auto& b : {task, group, wait})
        mixed.insert(mixed.end(), b.begin(), b.end());
    for (size_t at = 0; at < mixed.size();) {
        const auto n = std::min(size_t{37}, mixed.size() - at);
        assert(decoder.Feed(Chunk(mixed, 17, at, n, at, at + n == mixed.size()), out));
        at += n;
    }
    assert(out.size() == 3);
    assert(
        out[0].kind == RecordKind::CcuTask && out[1].kind == RecordKind::CcuGroup &&
        out[2].kind == RecordKind::CcuWait);
    assert(decoder.Finish());

    Decoder invalidLength, invalidMagic, truncated, gap;
    auto badLength = Additional(14, 47, 5500);
    assert(!invalidLength.Feed(Chunk(badLength, 17, 0, 256, 0, true), out));
    auto badMagic = task;
    Put(badMagic, 0, 0, 2);
    assert(!invalidMagic.Feed(Chunk(badMagic, 17, 0, 256, 0, true), out));
    assert(!truncated.Feed(Chunk(task, 17, 0, 55, 0, true), out));
    assert(gap.Feed(Chunk(task, 17, 0, 8, 0, false), out));
    assert(!gap.Feed(Chunk(task, 17, 8, 248, 9, true), out));
}
void CcuRuntimeTaskTrack()
{
    auto cc = Additional(802, 40, 5000);
    cc.resize(64);
    Put(cc, 24, 1, 2);
    Put(cc, 26, 60, 2);
    Put(cc, 28, 0x12345678, 4);
    Put(cc, 32, 30, 8);
    auto other = cc;
    Put(other, 32, 66, 8);
    Put(other, 26, UINT16_MAX, 2); // Non-CCU tracks are left to the application metadata reader.
    std::vector<Record> records;
    Decoder decoder;
    auto chunk = Chunk(other, 16, 0, other.size(), 0, true);
    chunk.deviceId = 64;
    assert(decoder.Feed(chunk, records));
    assert(records.empty());

    chunk = Chunk(cc, 16, 0, 21, 0, false);
    chunk.deviceId = 64;
    assert(decoder.Feed(chunk, records));
    assert(records.empty());
    chunk = Chunk(cc, 16, 21, cc.size() - 21, 21, true);
    chunk.deviceId = 64;
    assert(decoder.Feed(chunk, records));
    assert(decoder.Finish());
    assert(records.size() == 1);
    const auto& record = records.front();
    assert(record.kind == RecordKind::RuntimeTaskTrack && record.deviceId == 1);
    assert(record.streamId == 60 && record.taskId == 0x12345678);

    Decoder mismatchedDevice;
    chunk = Chunk(cc, 16, 0, cc.size(), 0, true);
    assert(!mismatchedDevice.Feed(chunk, records));
}
int main()
{
    AdditionalWhitelist();
    CcuRuntimeTaskTrack();
    Decoder d;
    std::vector<Record> out;
    for (auto type : {4U, 5U}) {
        const auto turn = Additional(type, type == 4 ? 80 : 48);
        assert(d.Feed(Chunk(turn, 12, 0, 256, 0, true), out));
    }
    assert(out.empty() && d.Finish());
    std::vector<uint8_t> c(64);
    Put(c, 0, 5, 2);
    Put(c, 32, 1000, 8);
    Put(c, 40, 44, 2);
    Put(c, 42, 300, 8);
    Put(c, 50, 200, 8);
    Put(c, 58, 45, 2);
    Put(c, 60, 0x12345678, 4);
    assert(d.Feed(Chunk(c, 13, 0, 64, 0, true), out));
    assert(out.back().taskId == 0x12345678 && out.back().streamId == UINT32_MAX);
    assert(out.back().times[0] == 200 && out.back().times[1] == 300 && out.back().dieId == 3);
    Decoder gap;
    assert(!gap.Feed(Chunk(c, 13, 0, 10, 1, true), out));
    Decoder partial;
    assert(partial.Feed(Chunk(c, 13, 0, 10, 0, true), out));
    assert(!partial.Finish());
    std::vector<uint8_t> channels(2816);
    Put(channels, 20, 3456, 8);
    Put(channels, 28, 73, 4);
    const auto channelBefore = out.size();
    for (size_t offset = 0; offset < channels.size(); offset += 256) {
        const auto size = std::min(size_t{256}, channels.size() - offset);
        assert(d.Feed(Chunk(channels, 14, offset, size, offset, offset + size == channels.size()), out));
    }
    assert(out.size() == channelBefore + 1 && d.Finish());
    assert(out.back().kind == RecordKind::CcuChannel && out.back().dieId == 3);
    assert(out.back().channelId == 1 && out.back().timestamp == 3456 && out.back().channelAverage == 73);
    auto hccl = Additional(13, 116);
    Put(hccl, 48, 8, 4);
    Put(hccl, 52, 9, 4);
    Put(hccl, 112, 0x12345678, 4);
    Put(hccl, 120, 13, 2);
    Put(hccl, 116, 42, 4);
    Put(hccl, 131, 0xfe, 1);
    assert(d.Feed(Chunk(hccl, 12, 0, 100, 0, false), out));
    assert(d.Feed(Chunk(hccl, 12, 100, 156, 100, true), out));
    assert(out.back().kind == RecordKind::HcclTask && out.back().taskId == 0x12345678 && out.back().rankId == 8);
    assert(out.back().reserve == 42 && out.back().reserves && out.back().reserves->front() == 0xfe);
    auto batch = Additional(13, 232);
    Put(batch, 24 + 88, 123, 4);
    Put(batch, 24 + 116 + 88, 456, 4);
    const auto batchBefore = out.size();
    assert(d.Feed(Chunk(batch, 12, 0, 256, 0, true), out));
    assert(out.size() == batchBefore + 2 && out[batchBefore].taskId == 123 && out.back().taskId == 456);
    auto legacy = Additional(6, 128);
    Put(legacy, 24 + 36, 2, 4);
    Put(legacy, 24 + 40, 3, 4);
    Put(legacy, 24 + 48, 99, 8);
    Put(legacy, 24 + 64, 0x3ff8000000000000ULL, 8);
    Put(legacy, 24 + 116, 0xabcdef12, 4);
    Put(legacy, 24 + 120, 33, 2);
    assert(d.Feed(Chunk(legacy, 12, 0, 256, 0, true), out));
    assert(out.back().workFlowMode == 2 && out.back().planeId == 3 && out.back().notifyId == 99);
    assert(out.back().durationEstimated == 1.5 && out.back().taskId == 0xabcdef12 && out.back().streamId == 33);
    assert(!out.back().reserve && !out.back().reserves);
    Decoder invalidBatch;
    auto wrongBatch = Additional(13, 128);
    assert(!invalidBatch.Feed(Chunk(wrongBatch, 12, 0, 256, 0, true), out));
    auto node = Additional(0, 128);
    Put(node, 80, 555, 8);
    Put(node, 142, 2, 1);
    assert(d.Feed(Chunk(node, 12, 0, 256, 0, true), out));
    assert(out.back().times[6] == 555 && out.back().version == 2);
    auto task = Additional(14, 48, 5500);
    Put(task, 56, 19, 2);
    Put(task, 60, 0x12345678, 4);
    Put(task, 64, 3, 1);
    Put(task, 66, 45, 2);
    assert(d.Feed(Chunk(task, 17, 0, 256, 0, true), out));
    assert(out.back().streamId == 19 && out.back().taskId == 0x12345678 && out.back().instrId == 45);
    auto group = Additional(16, 152, 5500);
    Put(group, 58, 19, 2);
    Put(group, 60, 22, 4);
    Put(group, 72, 9999, 8);
    Put(group, 110, 119, 2);
    Put(group, 172, 88, 4);
    assert(d.Feed(Chunk(group, 17, 0, 256, 0, true), out));
    assert(out.back().dataLen == 9999 && out.back().channelIds[15] == 119 && out.back().remoteRanks[15] == 88);
    auto wait = group;
    Put(wait, 4, 15, 4);
    Put(wait, 72, 22, 4);
    Put(wait, 76, 0xff, 4);
    assert(d.Feed(Chunk(wait, 17, 0, 256, 0, true), out));
    assert(out.back().ckeId == 22 && out.back().mask == 255);
    std::vector<uint8_t> log(32);
    Put(log, 0, 0x6bd30000, 4);
    Put(log, 4, 0x00420013, 4);
    Put(log, 8, 12345, 8);
    assert(d.Feed(Chunk(log, 7, 0, 32, 0, true), out));
    assert(
        out.back().kind == RecordKind::TaskStart && out.back().taskId == 0x00420013 &&
        out.back().streamId == UINT32_MAX);
    assert(out.back().taskType == 0 && out.back().functionType == 0);
    Put(log, 0, 0x6bd30001, 4);
    assert(d.Feed(Chunk(log, 7, 0, 32, 0, true), out));
    assert(out.back().kind == RecordKind::TaskEnd);
    // V6 SQE type 4 is AiCPU, and must not become a computational TaskLog.
    Put(log, 0, 0x6bd30000U | (4U << 10), 4);
    assert(d.Feed(Chunk(log, 7, 0, 32, 0, true), out));
    assert(out.back().taskType == 4 && out.back().functionType == 0);
    // Fusion TaskLog uses distinct 0x16/0x17 functions and a one-hot engine field.
    Put(log, 0, 0x6bd30016U | (2U << 10), 4);
    Put(log, 16, 4U | (3U << 5), 2);
    assert(d.Feed(Chunk(log, 7, 0, 32, 0, true), out));
    assert(out.back().kind == RecordKind::TaskStart && out.back().taskType == 2 && out.back().fusionType == 4);
    assert(out.back().missionId == 3 && out.back().functionType == 0x16);
    Put(log, 0, 0x6bd30017U | (2U << 10), 4);
    Put(log, 16, 8, 2);
    assert(d.Feed(Chunk(log, 7, 0, 32, 0, true), out));
    assert(out.back().kind == RecordKind::TaskEnd && out.back().fusionType == 8 && out.back().functionType == 0x17);
    Put(log, 0, 0x6bd30024, 4);
    Put(log, 20, 37U << 8, 2);
    Put(log, 24, (3U << 16) | 1U, 4);
    assert(d.Feed(Chunk(log, 7, 0, 32, 0, true), out));
    assert(out.back().kind == RecordKind::BlockStart && out.back().taskId == 0x00420013);
    assert(out.back().coreId == 37 && out.back().blockId == 3 && out.back().subBlockId == 1);
    Put(log, 0, 0x6bd30025, 4);
    assert(d.Feed(Chunk(log, 7, 0, 32, 0, true), out));
    assert(out.back().kind == RecordKind::BlockEnd);
    const auto before = out.size();
    assert(out.size() == before);
    assert(d.Feed(Chunk(log, 99, 0, 32, 0, true), out));
    assert(out.size() == before);
    Decoder huge;
    auto tooLong = Additional(13, 233);
    assert(!huge.Feed(Chunk(tooLong, 12, 0, 256, 0, true), out));
    Decoder overflow;
    Put(c, 32, UINT64_MAX, 8);
    assert(overflow.Feed(Chunk(c, 13, 0, 64, 0, true), out));
    Decoder unterminated;
    assert(unterminated.Feed(Chunk(log, 7, 0, 32, 0, false), out));
    assert(!unterminated.Finish());
    Decoder invalidMagic;
    Put(log, 0, 0, 4);
    assert(!invalidMagic.Feed(Chunk(log, 7, 0, 32, 0, true), out));
    assert(d.Finish());
    Decoder aiv;
    out.clear();
    auto timestamps = Additional(0, 48, 3000);
    Put(timestamps, 24, 267219224147085, 8);
    Put(timestamps, 32, (37U << 16) | 7U, 4);
    Put(timestamps, 36, 0x1010, 4);
    Put(timestamps, 40, 19792931100376, 8);
    Put(timestamps, 48, 267219224153843, 8);
    Put(timestamps, 56, (37U << 16) | 7U, 4);
    Put(timestamps, 60, 0x1011, 4);
    assert(aiv.Feed(Chunk(timestamps, 17, 0, 26, 0, false), out));
    assert(aiv.Feed(Chunk(timestamps, 17, 26, 230, 26, true), out));
    assert(out.size() == 2);
    assert(out[0].kind == RecordKind::AicoreTimestamp && out[0].descId == 0x1010);
    assert(out[0].times[Record::kTaskSystemCounter] == 267219224147085);
    assert(out[0].curPc == 19792931100376 && out[0].coreId == 37 && out[0].blockId == 7);
    assert(out.back().descId == 0x1011);
    std::cout << "communication decoder tests passed\n";
}
