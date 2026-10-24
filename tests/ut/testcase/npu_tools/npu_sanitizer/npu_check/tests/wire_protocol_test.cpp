// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "wire_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace npucheck::ipc {
namespace {

// 构造一个三工具请求；已有工具各带一个子选项，initcheck 不带专属选项。
ConfigureRequest SampleConfigure()
{
    ConfigureRequest request;
    ToolRequest memcheck;
    memcheck.toolId = ToolId::MEMCHECK;
    memcheck.options.push_back({OptionId::MEMCHECK_CHECK_CACHE_CONTROL, {0x01}});
    ToolRequest synccheck;
    synccheck.toolId = ToolId::SYNCCHECK;
    synccheck.options.push_back({OptionId::SYNCCHECK_MISSING_BARRIER_INIT_IS_FATAL, {0x01}});
    ToolRequest initcheck;
    initcheck.toolId = ToolId::INITCHECK;
    request.tools.push_back(std::move(memcheck));
    request.tools.push_back(std::move(synccheck));
    request.tools.push_back(std::move(initcheck));
    return request;
}

TEST(WireProtocolTest, RoundTripsHelloAndToolConfiguration)
{
    HelloPayload hello{};
    hello.pid = 123;
    hello.uid = 456;
    HelloPayload decodedHello{};
    std::string error;
    // Hello 固定 8 字节，不再携带 nonce。
    EXPECT_EQ(EncodeHello(hello).size(), 8U);
    ASSERT_TRUE(DecodeHello(EncodeHello(hello), decodedHello, error)) << error;
    EXPECT_EQ(decodedHello.pid, hello.pid);
    EXPECT_EQ(decodedHello.uid, hello.uid);

    // 尾随字节必须被拒绝：解码结束位置要恰好等于 payload_size。
    auto trailing = EncodeHello(hello);
    trailing.push_back(0);
    HelloPayload ignored{};
    EXPECT_FALSE(DecodeHello(trailing, ignored, error));

    const ConfigureRequest request = SampleConfigure();
    ConfigureRequest decoded{};
    ASSERT_TRUE(DecodeConfigure(EncodeConfigure(request), decoded, error)) << error;
    ASSERT_EQ(decoded.tools.size(), 3U);
    EXPECT_EQ(decoded.globalFlags, 0U);
    EXPECT_EQ(decoded.tools[0].toolId, ToolId::MEMCHECK);
    EXPECT_EQ(decoded.tools[1].toolId, ToolId::SYNCCHECK);
    EXPECT_EQ(decoded.tools[2].toolId, ToolId::INITCHECK);
    ASSERT_EQ(decoded.tools[0].options.size(), 1U);
    EXPECT_EQ(decoded.tools[0].options[0].optionId, OptionId::MEMCHECK_CHECK_CACHE_CONTROL);
    EXPECT_EQ(decoded.tools[0].options[0].value, std::vector<uint8_t>{0x01});
    ASSERT_EQ(decoded.tools[1].options.size(), 1U);
    EXPECT_EQ(decoded.tools[1].options[0].optionId, OptionId::SYNCCHECK_MISSING_BARRIER_INIT_IS_FATAL);
    EXPECT_TRUE(decoded.tools[2].options.empty());

    // 线路上不出现任何原始命令行字符串：布局是 4 字节头 + 每工具 4 字节 + 每选项 5 字节。
    EXPECT_EQ(EncodeConfigure(request).size(), 4U + 3U * 4U + 2U * 5U);
}

TEST(WireProtocolTest, ResolvesInitcheckToolNameWithoutChangingExistingIds)
{
    EXPECT_EQ(static_cast<uint16_t>(ToolId::MEMCHECK), 1U);
    EXPECT_EQ(static_cast<uint16_t>(ToolId::SYNCCHECK), 2U);
    EXPECT_EQ(static_cast<uint16_t>(ToolId::INITCHECK), 3U);
    EXPECT_STREQ(ToolName(ToolId::INITCHECK), "initcheck");
    ToolId id = ToolId::MEMCHECK;
    ASSERT_TRUE(LookupTool("initcheck", id));
    EXPECT_EQ(id, ToolId::INITCHECK);
}

TEST(WireProtocolTest, ResolvesOnlyRacecheckWithStableId)
{
    const auto racecheck = ToolId::RACECHECK;
    EXPECT_EQ(static_cast<uint16_t>(racecheck), 4U);
    EXPECT_STREQ(ToolName(racecheck), "racecheck");
    EXPECT_TRUE(IsKnownTool(4));
    ToolId id = ToolId::MEMCHECK;
    ASSERT_TRUE(LookupTool("racecheck", id));
    EXPECT_EQ(id, racecheck);
    EXPECT_FALSE(LookupTool("race-check", id));
    EXPECT_FALSE(IsKnownTool(5));
}

TEST(WireProtocolTest, RoundTripsRacecheckAloneAndWithExistingTools)
{
    const ToolRequest racecheck{ToolId::RACECHECK, {}};
    ConfigureRequest request;
    request.tools.push_back(racecheck);
    ConfigureRequest decoded;
    std::string error;
    ASSERT_TRUE(DecodeConfigure(EncodeConfigure(request), decoded, error)) << error;
    ASSERT_EQ(decoded.tools.size(), 1U);
    EXPECT_EQ(decoded.tools[0].toolId, racecheck.toolId);
    EXPECT_TRUE(decoded.tools[0].options.empty());

    request = SampleConfigure();
    request.tools.push_back(racecheck);
    ASSERT_TRUE(DecodeConfigure(EncodeConfigure(request), decoded, error)) << error;
    ASSERT_EQ(decoded.tools.size(), 4U);
    EXPECT_EQ(decoded.tools.back().toolId, racecheck.toolId);
    EXPECT_TRUE(ValidateConfigureMinor(decoded, 0, error)) << error;
    EXPECT_EQ(EncodeConfigure(request).size(), 4U + 4U * 4U + 2U * 5U);
}

TEST(WireProtocolTest, RejectsDuplicateUnsortedAndForeignOptionsForRacecheck)
{
    const ToolRequest racecheck{ToolId::RACECHECK, {}};
    ConfigureRequest request;
    ConfigureRequest decoded;
    std::string error;
    request.tools = {racecheck, racecheck};
    EXPECT_FALSE(DecodeConfigure(EncodeConfigure(request), decoded, error));
    request.tools = {racecheck, {ToolId::MEMCHECK, {}}};
    EXPECT_FALSE(DecodeConfigure(EncodeConfigure(request), decoded, error));
    for (const auto& entry : OptionRegistry()) {
        request.tools = {racecheck};
        request.tools[0].options.push_back({entry.optionId, {entry.presentValue}});
        EXPECT_FALSE(DecodeConfigure(EncodeConfigure(request), decoded, error));
        EXPECT_EQ(error, "option does not belong to the enclosing tool");
    }
}

// 规范化编码唯一：未排序或重复的 tool_id / option_id 必须被拒绝。接受了非规范编码，
// "子选项换个位置写、编码结果不变"这条性质就无法断言。
TEST(WireProtocolTest, RejectsNonCanonicalConfigure)
{
    ConfigureRequest decoded{};
    std::string error;

    ConfigureRequest unsorted;
    ToolRequest synccheck;
    synccheck.toolId = ToolId::SYNCCHECK;
    ToolRequest memcheck;
    memcheck.toolId = ToolId::MEMCHECK;
    unsorted.tools.push_back(std::move(synccheck));
    unsorted.tools.push_back(std::move(memcheck));
    EXPECT_FALSE(DecodeConfigure(EncodeConfigure(unsorted), decoded, error));

    ConfigureRequest duplicated;
    ToolRequest first;
    first.toolId = ToolId::MEMCHECK;
    ToolRequest second;
    second.toolId = ToolId::MEMCHECK;
    duplicated.tools.push_back(std::move(first));
    duplicated.tools.push_back(std::move(second));
    EXPECT_FALSE(DecodeConfigure(EncodeConfigure(duplicated), decoded, error));

    ConfigureRequest duplicatedOption;
    ToolRequest tool;
    tool.toolId = ToolId::MEMCHECK;
    tool.options.push_back({OptionId::MEMCHECK_CHECK_CACHE_CONTROL, {0x01}});
    tool.options.push_back({OptionId::MEMCHECK_CHECK_CACHE_CONTROL, {0x01}});
    duplicatedOption.tools.push_back(std::move(tool));
    EXPECT_FALSE(DecodeConfigure(EncodeConfigure(duplicatedOption), decoded, error));
}

TEST(WireProtocolTest, RejectsInvalidConfigureContent)
{
    ConfigureRequest decoded{};
    std::string error;

    // global_flags 非 0。
    auto flagged = EncodeConfigure(SampleConfigure());
    flagged[1] = 1;
    EXPECT_FALSE(DecodeConfigure(flagged, decoded, error));

    // 未知 tool_id。
    ConfigureRequest unknownTool;
    ToolRequest tool;
    tool.toolId = static_cast<ToolId>(0x0fff);
    unknownTool.tools.push_back(std::move(tool));
    EXPECT_FALSE(DecodeConfigure(EncodeConfigure(unknownTool), decoded, error));

    // 选项归属与所在工具不符：option_id 已唯一确定所属工具，静默接受会让选项
    // 作用到错误的 checker 上。
    ConfigureRequest wrongOwner;
    ToolRequest owner;
    owner.toolId = ToolId::MEMCHECK;
    owner.options.push_back({OptionId::SYNCCHECK_MISSING_BARRIER_INIT_IS_FATAL, {0x01}});
    wrongOwner.tools.push_back(std::move(owner));
    EXPECT_FALSE(DecodeConfigure(EncodeConfigure(wrongOwner), decoded, error));

    // 值域越界：V1 全部是"出现即为真"的开关。
    ConfigureRequest badValue;
    ToolRequest valued;
    valued.toolId = ToolId::MEMCHECK;
    valued.options.push_back({OptionId::MEMCHECK_CHECK_CACHE_CONTROL, {0x07}});
    badValue.tools.push_back(std::move(valued));
    EXPECT_FALSE(DecodeConfigure(EncodeConfigure(badValue), decoded, error));

    // 尾随字节。
    auto trailing = EncodeConfigure(SampleConfigure());
    trailing.push_back(0);
    EXPECT_FALSE(DecodeConfigure(trailing, decoded, error));
}

// introducedMinor 门禁：注册表里记录的引入版本高于协商版本时，该选项不得被接受。
// 两端都要查 —— 任何一端都可能是较旧的实现。
TEST(WireProtocolTest, RejectsOptionsAboveNegotiatedMinor)
{
    const ConfigureRequest request = SampleConfigure();
    std::string error;

    // V1 的两个选项 introducedMinor 均为 0，协商到 0 时全部可用。
    EXPECT_TRUE(ValidateConfigureMinor(request, 0, error)) << error;

    // 注册表里没有 introducedMinor > 0 的选项，因此这里用未知 optionId 覆盖另一条拒绝路径。
    ConfigureRequest unknown;
    ToolRequest tool;
    tool.toolId = ToolId::MEMCHECK;
    tool.options.push_back({static_cast<OptionId>(0x0999), {0x01}});
    unknown.tools.push_back(std::move(tool));
    EXPECT_FALSE(ValidateConfigureMinor(unknown, 0, error));

    // 空请求恒通过。
    EXPECT_TRUE(ValidateConfigureMinor(ConfigureRequest{}, 0, error)) << error;
}

TEST(WireProtocolTest, PreservesBigEndianWireRepresentation)
{
    Frame frame{};
    frame.type = MessageType::RESULT;
    frame.flags = FLAG_HAS_ERRORS;
    frame.sessionId = 0x0123456789abcdefULL;
    frame.sequence = 0xfedcba9876543210ULL;
    frame.payload = {0x80, 0xff};
    const std::vector<uint8_t> expected = {0x4e, 0x53, 0x41, 0x4e, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x02, 0x00,
                                           0x00, 0x00, 0x26, 0x00, 0x00, 0x00, 0x02, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
                                           0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10, 0x80, 0xff};
    EXPECT_EQ(EncodeFrame(frame), expected);
    Frame decoded{};
    std::string error;
    ASSERT_TRUE(DecodeFrame(expected.data(), expected.size(), decoded, error)) << error;
    EXPECT_EQ(decoded.flags, frame.flags);
    EXPECT_EQ(decoded.sessionId, frame.sessionId);
    EXPECT_EQ(decoded.sequence, frame.sequence);
    EXPECT_EQ(decoded.payload, frame.payload);
}

TEST(WireProtocolTest, RoundTripsFrame)
{
    Frame frame{};
    frame.type = MessageType::CONFIGURE;
    frame.flags = 3;
    frame.sessionId = 0x1020304050607080ULL;
    frame.sequence = 9;
    frame.payload = EncodeConfigure(SampleConfigure());

    const auto bytes = EncodeFrame(frame);
    Frame decoded{};
    std::string error;
    ASSERT_TRUE(DecodeFrame(bytes.data(), bytes.size(), decoded, error)) << error;
    EXPECT_EQ(decoded.type, frame.type);
    EXPECT_EQ(decoded.flags, frame.flags);
    EXPECT_EQ(decoded.sessionId, frame.sessionId);
    EXPECT_EQ(decoded.sequence, frame.sequence);
    EXPECT_EQ(decoded.payload, frame.payload);
}

TEST(WireProtocolTest, RejectsMalformedFramesAndText)
{
    Frame frame{};
    frame.type = MessageType::CONFIGURE;
    frame.sessionId = 1;
    frame.sequence = 1;
    const auto bytes = EncodeFrame(frame);
    Frame decoded{};
    std::string error;

    EXPECT_FALSE(DecodeFrame(bytes.data(), WIRE_HEADER_SIZE - 1, decoded, error));
    auto corrupt = bytes;
    corrupt[0] = 0;
    EXPECT_FALSE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error));
    // major 不匹配即拒绝（偏移 4）。注意这里不能拿 minor（偏移 6）做同样的断言 ——
    // minor 偏高是合法的，见 AcceptsHigherPeerMinorAndNegotiatesDown。
    corrupt = bytes;
    corrupt[4] = 0;
    corrupt[5] = 2;
    EXPECT_FALSE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error));
    corrupt = bytes;
    corrupt.push_back(0);
    EXPECT_FALSE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error));

    std::string text;
    ASSERT_TRUE(DecodeText(EncodeText("hello"), text, error)) << error;
    EXPECT_EQ(text, "hello");
    const std::vector<uint8_t> invalidText{'a', 0, 'b'};
    EXPECT_FALSE(DecodeText(invalidText, text, error));
}

// 帧头偏移常量，与 EncodeFrame 的布局一一对应。
constexpr size_t OFFSET_TYPE = 8;
constexpr size_t OFFSET_FLAGS = 10;
constexpr size_t OFFSET_LENGTH = 12;
constexpr size_t OFFSET_PAYLOAD_SIZE = 16;

void PutU16At(std::vector<uint8_t>& bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<uint8_t>(value & 0xffu);
}

void PutU32At(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    for (size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> ((3 - index) * 8));
    }
}

std::vector<uint8_t> SampleFrame()
{
    Frame frame{};
    frame.type = MessageType::READY;
    frame.sessionId = 42;
    frame.sequence = 7;
    frame.payload = EncodeText("payload");
    return EncodeFrame(frame);
}

TEST(WireProtocolTest, HeaderIs36BytesAndCarriesLength)
{
    EXPECT_EQ(WIRE_HEADER_SIZE, 36U);
    const auto bytes = SampleFrame();
    Frame decoded{};
    std::string error;
    ASSERT_TRUE(DecodeFrame(bytes.data(), bytes.size(), decoded, error)) << error;
    // length 必须等于整帧长度，也就是 36 + payload_size。
    const size_t payloadSize = bytes.size() - WIRE_HEADER_SIZE;
    uint32_t length = 0;
    for (size_t index = 0; index < 4; ++index) {
        length = (length << 8) | bytes[OFFSET_LENGTH + index];
    }
    EXPECT_EQ(length, WIRE_HEADER_SIZE + payloadSize);
}

TEST(WireProtocolTest, RejectsInconsistentLength)
{
    std::string error;
    Frame decoded{};

    // length 与 payload_size 对不上。
    auto corrupt = SampleFrame();
    PutU32At(corrupt, OFFSET_LENGTH, static_cast<uint32_t>(corrupt.size() + 1));
    EXPECT_FALSE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error));

    // length 与实际收到的数据报长度对不上（两者都改，绕过上一条校验）。
    corrupt = SampleFrame();
    const uint32_t shorter = static_cast<uint32_t>(corrupt.size() - 1);
    PutU32At(corrupt, OFFSET_LENGTH, shorter);
    PutU32At(corrupt, OFFSET_PAYLOAD_SIZE, static_cast<uint32_t>(shorter - WIRE_HEADER_SIZE));
    EXPECT_FALSE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error));
}

TEST(WireProtocolTest, RejectsReservedMustUnderstandFlags)
{
    std::string error;
    Frame decoded{};

    // bit 3-7 任一置位即协议错误。
    auto corrupt = SampleFrame();
    PutU16At(corrupt, OFFSET_FLAGS, static_cast<uint16_t>(0x0008u));
    EXPECT_FALSE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error));
    corrupt = SampleFrame();
    PutU16At(corrupt, OFFSET_FLAGS, static_cast<uint16_t>(0x0080u));
    EXPECT_FALSE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error));

    // bit 8-15 是 must-ignore 的保留位，必须被忽略而不是报错。
    corrupt = SampleFrame();
    PutU16At(corrupt, OFFSET_FLAGS, static_cast<uint16_t>(FLAG_HAS_ERRORS | 0x0100u));
    ASSERT_TRUE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error)) << error;
    EXPECT_NE(decoded.flags & FLAG_HAS_ERRORS, 0);
}

// 收到比自己高的 minor 不是错误：同一 major 下帧头格式稳定，拒绝会让新版本永远
// 连不上旧版本。解码必须成功，并把对端 minor 原样带回给上层协商。
TEST(WireProtocolTest, AcceptsHigherPeerMinorAndNegotiatesDown)
{
    Frame frame{};
    frame.type = MessageType::READY;
    frame.sessionId = 7;
    frame.sequence = 2;
    auto bytes = EncodeFrame(frame);
    ASSERT_FALSE(bytes.empty());
    // 把帧头偏移 6 处的 minor 改成一个远高于本实现的值。
    bytes[6] = 0x00;
    bytes[7] = 0x09;

    Frame decoded{};
    std::string error;
    ASSERT_TRUE(DecodeFrame(bytes.data(), bytes.size(), decoded, error)) << error;
    EXPECT_EQ(decoded.minor, 9U);
    EXPECT_EQ(NegotiateMinor(decoded.minor), PROTOCOL_MINOR);
    // 对端更低时取对端的值。
    EXPECT_EQ(NegotiateMinor(0U), 0U);
}

TEST(WireProtocolTest, RoundTripsErrorPayload)
{
    ErrorPayload payload{};
    payload.domain = ErrorDomain::CONFIGURATION;
    payload.code = error_code::CONFIGURE_MALFORMED;
    payload.message = "unsorted tool_id";

    const auto bytes = EncodeError(payload);
    ASSERT_EQ(bytes.size(), ERROR_HEADER_SIZE + payload.message.size());

    ErrorPayload decoded{};
    std::string error;
    ASSERT_TRUE(DecodeError(bytes, decoded, error)) << error;
    EXPECT_EQ(decoded.domain, ErrorDomain::CONFIGURATION);
    EXPECT_EQ(decoded.code, error_code::CONFIGURE_MALFORMED);
    EXPECT_EQ(decoded.message, "unsorted tool_id");
}

TEST(WireProtocolTest, RejectsMalformedErrorPayload)
{
    ErrorPayload decoded{};
    std::string error;

    // 短于 8 字节的固定头。
    EXPECT_FALSE(DecodeError(std::vector<uint8_t>(4, 0), decoded, error));

    auto valid = EncodeError({ErrorDomain::PROTOCOL, error_code::FRAME_REJECTED, "bad sequence"});
    // reserved 非零。
    auto reservedSet = valid;
    reservedSet[7] = 1;
    EXPECT_FALSE(DecodeError(reservedSet, decoded, error));

    // 声明长度与实际尾随字节不符。
    auto trailing = valid;
    trailing.push_back('x');
    EXPECT_FALSE(DecodeError(trailing, decoded, error));

    // 未知 domain。
    auto unknownDomain = valid;
    unknownDomain[1] = 9;
    EXPECT_FALSE(DecodeError(unknownDomain, decoded, error));
}

// 超长文案截断而不是整体失败：Error 是"出事了"的通道，编不出帧等于把最需要的信息丢掉。
TEST(WireProtocolTest, TruncatesOverlongErrorMessage)
{
    ErrorPayload payload{};
    payload.domain = ErrorDomain::INTERNAL;
    payload.message.assign(MAX_ERROR_MESSAGE_SIZE + 100, 'x');

    const auto bytes = EncodeError(payload);
    ErrorPayload decoded{};
    std::string error;
    ASSERT_TRUE(DecodeError(bytes, decoded, error)) << error;
    EXPECT_EQ(decoded.message.size(), MAX_ERROR_MESSAGE_SIZE);
}

TEST(WireProtocolTest, AppliesMustIgnoreRuleToUnknownTypes)
{
    std::string error;
    Frame decoded{};

    // 最高位为 0 的未知类型 → 协议错误。
    auto corrupt = SampleFrame();
    PutU16At(corrupt, OFFSET_TYPE, static_cast<uint16_t>(0x0055u));
    EXPECT_FALSE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error));

    // 最高位为 1 的未知类型 → 正常解出，交由上层跳过。
    corrupt = SampleFrame();
    PutU16At(corrupt, OFFSET_TYPE, static_cast<uint16_t>(0x80abu));
    ASSERT_TRUE(DecodeFrame(corrupt.data(), corrupt.size(), decoded, error)) << error;
    EXPECT_TRUE(IsMustIgnoreType(static_cast<uint16_t>(decoded.type)));

    // 我们自己定义的实时诊断类型也落在 must-ignore 区间。
    EXPECT_TRUE(IsMustIgnoreType(static_cast<uint16_t>(MessageType::DIAGNOSTIC_STREAM)));
    EXPECT_FALSE(IsMustIgnoreType(static_cast<uint16_t>(MessageType::READY)));
}

} // namespace
} // namespace npucheck::ipc
