/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "wire_protocol.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace npucheck::ipc {
namespace {

constexpr unsigned BITS_PER_WIRE_BYTE = 8;

class Encoder {
public:
    void PutU8(uint8_t value) { data_.push_back(value); }

    void PutU16(uint16_t value)
    {
        data_.push_back(static_cast<uint8_t>(value >> BITS_PER_WIRE_BYTE));
        data_.push_back(static_cast<uint8_t>(value));
    }

    void PutU32(uint32_t value)
    {
        for (size_t remaining = sizeof(value); remaining > 0; --remaining) {
            data_.push_back(static_cast<uint8_t>(value >> ((remaining - 1) * BITS_PER_WIRE_BYTE)));
        }
    }

    void PutU64(uint64_t value)
    {
        for (size_t remaining = sizeof(value); remaining > 0; --remaining) {
            data_.push_back(static_cast<uint8_t>(value >> ((remaining - 1) * BITS_PER_WIRE_BYTE)));
        }
    }

    bool PutString(const std::string& value)
    {
        if (value.size() > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        PutU32(static_cast<uint32_t>(value.size()));
        data_.insert(data_.end(), value.begin(), value.end());
        return true;
    }

    void PutBytes(const std::vector<uint8_t>& value) { data_.insert(data_.end(), value.begin(), value.end()); }

    std::vector<uint8_t> Take() { return std::move(data_); }

private:
    std::vector<uint8_t> data_;
};

class Decoder {
public:
    Decoder(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool GetU8(uint8_t& value)
    {
        if (!Need(1)) {
            return false;
        }
        value = data_[offset_++];
        return true;
    }

    bool GetU16(uint16_t& value)
    {
        if (!Need(sizeof(value))) {
            return false;
        }
        value = static_cast<uint16_t>(
            (static_cast<uint16_t>(data_[offset_]) << BITS_PER_WIRE_BYTE) | static_cast<uint16_t>(data_[offset_ + 1]));
        offset_ += sizeof(value);
        return true;
    }

    bool GetU32(uint32_t& value)
    {
        if (!Need(sizeof(value))) {
            return false;
        }
        uint32_t result = 0;
        for (size_t i = 0; i < sizeof(value); ++i) {
            result = (result << BITS_PER_WIRE_BYTE) | data_[offset_ + i];
        }
        offset_ += sizeof(value);
        value = result;
        return true;
    }

    bool GetU64(uint64_t& value)
    {
        if (!Need(sizeof(value))) {
            return false;
        }
        uint64_t result = 0;
        for (size_t i = 0; i < sizeof(value); ++i) {
            result = (result << BITS_PER_WIRE_BYTE) | data_[offset_ + i];
        }
        offset_ += sizeof(value);
        value = result;
        return true;
    }

    bool GetString(std::string& value, size_t maxSize)
    {
        uint32_t length = 0;
        if (!GetU32(length) || length > maxSize || !Need(length)) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return true;
    }

    // 定长字节串。长度由调用方（注册表）给出，线路上不再重复携带。
    bool GetBytes(std::vector<uint8_t>& value, size_t length)
    {
        if (!Need(length)) {
            return false;
        }
        value.assign(data_ + offset_, data_ + offset_ + length);
        offset_ += length;
        return true;
    }

    bool Done() const { return offset_ == size_; }

private:
    bool Need(size_t bytes) const { return bytes <= size_ - offset_; }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
};

bool Fail(std::string& error, const char* message)
{
    error = message;
    return false;
}

// 本实现认识的 must-understand 类型（最高位为 0 的那些）。收到不在此列的
// must-understand 类型必须判为协议错误，而不是跳过。
bool IsKnownMustUnderstandType(uint16_t type)
{
    switch (static_cast<MessageType>(type)) {
        case MessageType::CLIENT_HELLO:
        case MessageType::SERVER_HELLO:
        case MessageType::CONFIGURE:
        case MessageType::READY:
        case MessageType::RESULT:
        case MessageType::ERROR:
            return true;
        case MessageType::DIAGNOSTIC_STREAM:
            // 归 must-ignore 区间管，走不到这里。
            return false;
    }
    return false;
}

} // namespace

std::vector<uint8_t> EncodeFrame(const Frame& frame)
{
    if (frame.payload.size() > kMaxPayloadSize) {
        return {};
    }
    // 帧头布局（偏移 / 字段 / 宽度）：
    //   0  magic        u32     4  major  u16     6  minor u16
    //   8  type         u16    10  flags  u16
    //  12  length       u32   —— 整帧字节数 = kWireHeaderSize + payload_size
    //  16  payload_size u32
    //  20  session_id   u64    28  sequence u64    36  payload
    //
    // 注意 session_id 落在偏移 20、sequence 落在 28，都不在 8 字节边界上。这对线路格式
    // 没有影响（各字段连续写入、不含 padding），但实现必须逐字段读写，不得把帧头映射成
    // C 结构体再 memcpy。
    Encoder encoder;
    encoder.PutU32(kProtocolMagic);
    encoder.PutU16(kProtocolMajor);
    encoder.PutU16(kProtocolMinor);
    encoder.PutU16(static_cast<uint16_t>(frame.type));
    encoder.PutU16(frame.flags);
    encoder.PutU32(static_cast<uint32_t>(kWireHeaderSize + frame.payload.size()));
    encoder.PutU32(static_cast<uint32_t>(frame.payload.size()));
    encoder.PutU64(frame.sessionId);
    encoder.PutU64(frame.sequence);
    auto output = encoder.Take();
    output.insert(output.end(), frame.payload.begin(), frame.payload.end());
    return output;
}

bool DecodeFrame(const uint8_t* data, size_t size, Frame& frame, std::string& error)
{
    if (data == nullptr || size < kWireHeaderSize) {
        return Fail(error, "short frame header");
    }
    Decoder decoder(data, kWireHeaderSize);
    uint32_t magic = 0;
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t type = 0;
    uint16_t flags = 0;
    uint32_t length = 0;
    uint32_t payloadSize = 0;
    uint64_t sessionId = 0;
    uint64_t sequence = 0;
    if (!decoder.GetU32(magic) || !decoder.GetU16(major) || !decoder.GetU16(minor) || !decoder.GetU16(type) ||
        !decoder.GetU16(flags) || !decoder.GetU32(length) || !decoder.GetU32(payloadSize) ||
        !decoder.GetU64(sessionId) || !decoder.GetU64(sequence)) {
        return Fail(error, "malformed frame header");
    }
    if (magic != kProtocolMagic) {
        return Fail(error, "invalid protocol magic");
    }
    if (major != kProtocolMajor) {
        return Fail(error, "incompatible protocol major version");
    }
    // minor 不参与拒绝判定：同一 major 下帧头格式稳定，收到比自己高的 minor 只说明对端
    // 更新，不说明这一帧无法解析。原样带回给上层去 NegotiateMinor，高位 minor 新增的
    // 消息类型和标志位分别由 must-ignore 规则兜住。
    // 曾经这里是 `minor > kProtocolMinor 即拒绝`，那会让新版本永远连不上旧版本。
    // length 与 payload_size 表达的是同一信息（相差常数 kWireHeaderSize），任何一侧编码
    // 出错都会让它们矛盾，因此必须强制交叉校验。三条都在分配 payload 缓冲区之前执行。
    if (payloadSize > kMaxPayloadSize) {
        return Fail(error, "invalid frame payload size");
    }
    if (length != kWireHeaderSize + payloadSize) {
        return Fail(error, "frame length does not match payload size");
    }
    // SOCK_SEQPACKET 保留消息边界，length 必然等于收到的数据报长度；不等说明对端实现
    // 有 bug，或者这根本不是一个完整的帧。
    if (size != length) {
        return Fail(error, "frame length does not match the received datagram");
    }
    // bit 3-7 是 must-understand 的保留位，非零即协议错误。bit 8-15 是 must-ignore 的
    // 保留位，这里不做任何校验，原样交给上层忽略。
    if ((flags & kFlagsReservedMustUnderstand) != 0) {
        return Fail(error, "reserved must-understand flag bits are set");
    }
    // 未知类型的处理取决于最高位：must-understand 直接拒绝，must-ignore 照常解出来，
    // 由上层跳过。这样 minor 版本才可能在不破坏旧实现的前提下新增消息。
    if (!IsMustIgnoreType(type) && !IsKnownMustUnderstandType(type)) {
        return Fail(error, "unknown message type");
    }
    frame.type = static_cast<MessageType>(type);
    frame.flags = flags;
    frame.minor = minor;
    frame.sessionId = sessionId;
    frame.sequence = sequence;
    frame.payload.assign(data + kWireHeaderSize, data + size);
    return true;
}

std::vector<uint8_t> EncodeHello(const HelloPayload& hello)
{
    // 固定 8 字节：pid u32 + uid u32。
    Encoder encoder;
    encoder.PutU32(hello.pid);
    encoder.PutU32(hello.uid);
    return encoder.Take();
}

bool DecodeHello(const std::vector<uint8_t>& payload, HelloPayload& hello, std::string& error)
{
    Decoder decoder(payload.data(), payload.size());
    // Done() 保证解码结束位置恰好等于 payload_size，不允许尾随字节。
    if (!decoder.GetU32(hello.pid) || !decoder.GetU32(hello.uid) || !decoder.Done()) {
        return Fail(error, "malformed hello payload");
    }
    return true;
}

bool ValidateConfigureMinor(const ConfigureRequest& request, uint16_t negotiatedMinor, std::string& error)
{
    for (const auto& tool : request.tools) {
        for (const auto& option : tool.options) {
            const OptionRegistryEntry* entry = LookupOptionById(option.optionId);
            if (entry == nullptr) {
                error = "unknown option id in configure";
                return false;
            }
            if (entry->introducedMinor > negotiatedMinor) {
                error = std::string("option '--") + entry->name + "' requires protocol minor " +
                        std::to_string(entry->introducedMinor) + " but the session negotiated " +
                        std::to_string(negotiatedMinor);
                return false;
            }
        }
    }
    return true;
}

std::vector<uint8_t> EncodeConfigure(const ConfigureRequest& request)
{
    // 布局：global_flags u16 / tool_count u16
    //       每工具：tool_id u16 / option_count u16
    //               每选项：option_id u16 / value_size u16 / value[value_size]
    //
    // 线路上不出现值类型字段，也不出现任何原始命令行字符串：option_id 在共享注册表里
    // 唯一确定所属工具、值编码与校验规则，两端查同一张表即可。
    if (request.tools.size() > std::numeric_limits<uint16_t>::max()) {
        return {};
    }
    Encoder encoder;
    encoder.PutU16(request.globalFlags);
    encoder.PutU16(static_cast<uint16_t>(request.tools.size()));
    for (const auto& tool : request.tools) {
        if (tool.options.size() > std::numeric_limits<uint16_t>::max()) {
            return {};
        }
        encoder.PutU16(static_cast<uint16_t>(tool.toolId));
        encoder.PutU16(static_cast<uint16_t>(tool.options.size()));
        for (const auto& option : tool.options) {
            if (option.value.size() > std::numeric_limits<uint16_t>::max()) {
                return {};
            }
            encoder.PutU16(static_cast<uint16_t>(option.optionId));
            encoder.PutU16(static_cast<uint16_t>(option.value.size()));
            encoder.PutBytes(option.value);
        }
    }
    auto payload = encoder.Take();
    return payload.size() <= kMaxPayloadSize ? payload : std::vector<uint8_t>{};
}

bool DecodeConfigure(const std::vector<uint8_t>& payload, ConfigureRequest& request, std::string& error)
{
    Decoder decoder(payload.data(), payload.size());
    uint16_t toolCount = 0;
    if (!decoder.GetU16(request.globalFlags) || !decoder.GetU16(toolCount)) {
        return Fail(error, "malformed configure header");
    }
    if (request.globalFlags != 0) {
        return Fail(error, "unsupported configure global flags");
    }
    request.tools.clear();
    request.tools.reserve(toolCount);

    // 严格递增即同时覆盖"已排序"与"不重复"两条约束，因此不需要额外的集合去重。
    // 拒绝未排序或重复的编码不是洁癖：规范化编码唯一是"子选项换个位置写、编码结果
    // 不变"这条性质的前提，接受了非规范编码，这条性质就无法断言。
    bool hasPreviousTool = false;
    uint16_t previousToolId = 0;
    for (uint16_t toolIndex = 0; toolIndex < toolCount; ++toolIndex) {
        uint16_t toolId = 0;
        uint16_t optionCount = 0;
        if (!decoder.GetU16(toolId) || !decoder.GetU16(optionCount)) {
            return Fail(error, "malformed tool entry");
        }
        if (hasPreviousTool && toolId <= previousToolId) {
            return Fail(error, "configure tools are not sorted or contain duplicates");
        }
        if (!IsKnownTool(toolId)) {
            return Fail(error, "unknown tool id in configure");
        }
        hasPreviousTool = true;
        previousToolId = toolId;

        ToolRequest tool;
        tool.toolId = static_cast<ToolId>(toolId);
        tool.options.reserve(optionCount);
        bool hasPreviousOption = false;
        uint16_t previousOptionId = 0;
        for (uint16_t optionIndex = 0; optionIndex < optionCount; ++optionIndex) {
            uint16_t optionId = 0;
            uint16_t valueSize = 0;
            if (!decoder.GetU16(optionId) || !decoder.GetU16(valueSize)) {
                return Fail(error, "malformed option entry");
            }
            if (hasPreviousOption && optionId <= previousOptionId) {
                return Fail(error, "configure options are not sorted or contain duplicates");
            }
            hasPreviousOption = true;
            previousOptionId = optionId;

            const OptionRegistryEntry* entry = LookupOptionById(static_cast<OptionId>(optionId));
            if (entry == nullptr) {
                return Fail(error, "unknown option id in configure");
            }
            // 归属必须与所在的 tool 一致：option_id 已经唯一确定了所属工具，两者不符
            // 说明对端编码有问题，静默接受会让选项作用到错误的 checker 上。
            if (entry->toolId != tool.toolId) {
                return Fail(error, "option does not belong to the enclosing tool");
            }
            if (valueSize != entry->valueSize) {
                return Fail(error, "option value size does not match the registry");
            }
            OptionValue option;
            option.optionId = static_cast<OptionId>(optionId);
            if (!decoder.GetBytes(option.value, valueSize)) {
                return Fail(error, "truncated option value");
            }
            // V1 全部是"出现即为真"的开关，值域只有 presentValue 一个合法取值。
            if (option.value.size() != 1 || option.value[0] != entry->presentValue) {
                return Fail(error, "option value is out of range");
            }
            tool.options.push_back(std::move(option));
        }
        request.tools.push_back(std::move(tool));
    }
    if (!decoder.Done()) {
        return Fail(error, "trailing configure data");
    }
    return true;
}

std::vector<uint8_t> EncodeError(const ErrorPayload& payload)
{
    // 布局：domain u16 / code u16 / message_size u16 / reserved u16 / message[message_size]
    //
    // 超长文案截断而不是整体失败：Error 本身就是"出事了"的通道，若因为文案太长而编不出
    // 帧，对端就只剩连接关闭可看，反而丢掉了最需要的那点信息。
    const size_t messageSize = std::min(payload.message.size(), kMaxErrorMessageSize);
    Encoder encoder;
    encoder.PutU16(static_cast<uint16_t>(payload.domain));
    encoder.PutU16(payload.code);
    encoder.PutU16(static_cast<uint16_t>(messageSize));
    encoder.PutU16(0); // reserved
    auto bytes = encoder.Take();
    bytes.insert(
        bytes.end(), payload.message.begin(), payload.message.begin() + static_cast<std::ptrdiff_t>(messageSize));
    return bytes;
}

bool DecodeError(const std::vector<uint8_t>& payload, ErrorPayload& decoded, std::string& error)
{
    Decoder decoder(payload.data(), payload.size());
    uint16_t domain = 0;
    uint16_t code = 0;
    uint16_t messageSize = 0;
    uint16_t reserved = 0;
    if (!decoder.GetU16(domain) || !decoder.GetU16(code) || !decoder.GetU16(messageSize) || !decoder.GetU16(reserved)) {
        return Fail(error, "short error payload");
    }
    if (reserved != 0) {
        return Fail(error, "reserved error field is not zero");
    }
    if (messageSize > kMaxErrorMessageSize) {
        return Fail(error, "error message exceeds the maximum size");
    }
    // 8 字节头之后必须恰好剩 message_size 字节，不允许尾随数据。
    if (payload.size() != kErrorHeaderSize + messageSize) {
        return Fail(error, "error payload length does not match the declared message size");
    }
    if (domain < static_cast<uint16_t>(ErrorDomain::INJECTION) ||
        domain > static_cast<uint16_t>(ErrorDomain::INTERNAL)) {
        return Fail(error, "unknown error domain");
    }
    decoded.domain = static_cast<ErrorDomain>(domain);
    decoded.code = code;
    decoded.message.assign(payload.begin() + static_cast<std::ptrdiff_t>(kErrorHeaderSize), payload.end());
    return true;
}

std::vector<uint8_t> EncodeText(const std::string& text)
{
    if (text.size() > kMaxPayloadSize) {
        return {};
    }
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool DecodeText(const std::vector<uint8_t>& payload, std::string& text, std::string& error)
{
    for (uint8_t value : payload) {
        if (value == 0) {
            return Fail(error, "text payload contains NUL");
        }
    }
    text.assign(payload.begin(), payload.end());
    return true;
}

const char* MessageTypeName(MessageType type)
{
    switch (type) {
        case MessageType::CLIENT_HELLO:
            return "CLIENT_HELLO";
        case MessageType::SERVER_HELLO:
            return "SERVER_HELLO";
        case MessageType::CONFIGURE:
            return "CONFIGURE";
        case MessageType::READY:
            return "READY";
        case MessageType::RESULT:
            return "RESULT";
        case MessageType::ERROR:
            return "ERROR";
        case MessageType::DIAGNOSTIC_STREAM:
            return "DIAGNOSTIC";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 选项注册表
// ---------------------------------------------------------------------------

const char* ToolName(ToolId id)
{
    switch (id) {
        case ToolId::MEMCHECK:
            return "memcheck";
        case ToolId::SYNCCHECK:
            return "synccheck";
        case ToolId::INITCHECK:
            return "initcheck";
        case ToolId::RACECHECK:
            return "racecheck";
    }
    return "unknown";
}

bool LookupTool(const std::string& name, ToolId& id)
{
    if (name == "memcheck") {
        id = ToolId::MEMCHECK;
        return true;
    }
    if (name == "synccheck") {
        id = ToolId::SYNCCHECK;
        return true;
    }
    if (name == "initcheck") {
        id = ToolId::INITCHECK;
        return true;
    }
    if (name == "racecheck") {
        id = ToolId::RACECHECK;
        return true;
    }
    return false;
}

const std::vector<OptionRegistryEntry>& OptionRegistry()
{
    // 当前全部是"出现即为真"的布尔开关：value_size=1 且只接受 0x01；
    // 缺省时不发送 OptionValue，由 Server 取注册表默认值。
    static const std::vector<OptionRegistryEntry> registry = {
        {"check-cache-control", OptionId::MEMCHECK_CHECK_CACHE_CONTROL, ToolId::MEMCHECK, 0, 1, 0x01},
        {"missing-barrier-init-is-fatal", OptionId::SYNCCHECK_MISSING_BARRIER_INIT_IS_FATAL, ToolId::SYNCCHECK, 0, 1,
         0x01},
    };
    return registry;
}

const OptionRegistryEntry* LookupOption(const std::string& name)
{
    for (const auto& entry : OptionRegistry()) {
        if (name == entry.name) {
            return &entry;
        }
    }
    return nullptr;
}

const OptionRegistryEntry* LookupOptionById(OptionId optionId)
{
    for (const auto& entry : OptionRegistry()) {
        if (optionId == entry.optionId) {
            return &entry;
        }
    }
    return nullptr;
}

bool IsKnownTool(uint16_t toolId)
{
    switch (static_cast<ToolId>(toolId)) {
        case ToolId::MEMCHECK:
        case ToolId::SYNCCHECK:
        case ToolId::INITCHECK:
        case ToolId::RACECHECK:
            return true;
    }
    return false;
}

} // namespace npucheck::ipc
