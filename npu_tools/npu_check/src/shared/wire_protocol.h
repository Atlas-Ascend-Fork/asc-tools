/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_SHARED_WIRE_PROTOCOL_H
#define NPU_TOOLS_NPU_CHECK_SRC_SHARED_WIRE_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace npucheck::ipc {

constexpr uint32_t kProtocolMagic = 0x4e53414eu;
constexpr uint16_t kProtocolMajor = 1;
constexpr uint16_t kProtocolMinor = 0;
// 帧头固定 36 字节，布局见下方 EncodeFrame。字段顺序、宽度与偏移属于 wire ABI，
// 增删字段或改变偏移都是 major 变更。
constexpr size_t kWireHeaderSize = 36;
constexpr size_t kMaxPayloadSize = 64 * 1024;
constexpr size_t kMaxFrameSize = kWireHeaderSize + kMaxPayloadSize; // 65572
// 单次会话的报告总长上限。两侧共用同一个常量：发送侧据此决定何时停止追加并置
// kFlagTruncated，接收侧据此拒绝超量的分片序列，避免对端异常时把 CLI 的内存吃光。
constexpr size_t kMaxResultBytes = 64u * 1024u * 1024u;

// 帧头 flags 的位定义。
//
// bit 0-2 是 must-understand 的语义位，bit 3-7 保留且必须为 0（接收端发现非零即协议
// 错误），bit 8-15 保留给未来 minor 新增的 must-ignore 标志（接收端必须忽略无法识别
// 的置位）。按位区分这两类，使得 minor 版本可以在不破坏旧实现的前提下新增标志。
constexpr uint16_t kFlagMore = 1u << 0;      // 仅 Result 有效：同一报告还有续帧
constexpr uint16_t kFlagHasErrors = 1u << 1; // 仅 Result 末帧有效：本次检查发现了问题
constexpr uint16_t kFlagTruncated = 1u << 2; // 仅 Result 末帧有效：报告因触及总长上限被截断
constexpr uint16_t kFlagsReservedMustUnderstand = 0x00f8u; // bit 3-7
constexpr uint16_t kFlagsReservedMustIgnore = 0xff00u;     // bit 8-15

// UDS 地址走 Linux 抽象命名空间，不在文件系统里创建 socket 文件，因此这里是"名字"
// 而不是"路径"。取值形如 @npu_check-<32 位十六进制随机数>：前导 '@' 只是便于人读，
// 也与 ss -x、/proc/net/unix 的显示一致，两端填充 sockaddr_un 时必须把它换成 '\0'。
// 具体填充与 addrLen 计算见 uds_transport.h 的 BuildAbstractAddress。
constexpr const char* kUdsNameEnv = "NPU_CHECK_UDS_NAME";
constexpr const char* kSessionIdEnv = "NPU_CHECK_SESSION_ID";
constexpr const char* kCliPidEnv = "NPU_CHECK_CLI_PID";
constexpr const char* kHandshakeTimeoutEnv = "NPU_CHECK_HANDSHAKE_TIMEOUT_MS";
constexpr int MIN_HANDSHAKE_TIMEOUT_MS = 100;
constexpr int MAX_HANDSHAKE_TIMEOUT_MS = 120000;
// 工作目录：probe 缓存落在这里。改用注册表编码之后，
// Configure 只承载工具与子选项，不再承载路径，因此这类字段改由环境变量传递。
constexpr const char* kWorkDirEnv = "NPU_CHECK_WORK_DIR";

// 消息类型。
//
// 最高位（0x8000）表示该消息是否可选：置 0 为 must-understand，收到未知类型即协议
// 错误；置 1 为 must-ignore，收到未知类型必须跳过并继续。这条规则让 minor 版本可以
// 新增消息而不破坏旧实现。
enum class MessageType : uint16_t {
    CLIENT_HELLO = 0x0001,
    SERVER_HELLO = 0x0002,
    CONFIGURE = 0x0003,
    READY = 0x0004,
    // 本次检查的权威报告，也是唯一的报告出口。报告可能远大于单帧上限，因此以分片序列
    // 发送：除末帧外都置 kFlagMore，结论位只在末帧上有效。
    RESULT = 0x0005,
    ERROR = 0x00ff,

    // must-ignore 区间。实时诊断只供人阅读，不参与退出码判定；权威结论由 Result 承载。
    DIAGNOSTIC_STREAM = 0x8001,
};

// 未知类型是否可以安全跳过。
constexpr bool IsMustIgnoreType(uint16_t type) { return (type & 0x8000u) != 0; }

struct Frame {
    MessageType type = MessageType::ERROR;
    uint16_t flags = 0;
    // 对端在帧头声明的 minor。仅接收方向有意义；发送时一律写本实现的 kProtocolMinor。
    uint16_t minor = kProtocolMinor;
    uint64_t sessionId = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> payload;
};

// 协商 minor 取双方较小值。
//
// 注意这里"不拒绝"比"取较小值"更重要：同一 major 下 Hello 的基础格式保持稳定，因此
// 收到比自己高的 minor 绝不能判为错误。一旦拒绝，新版本就永远无法与旧版本互通，minor
// 这个字段也就失去了存在意义 —— 它的全部用途就是让两端在不同版本下仍能谈拢一个共同
// 子集。高位 minor 新增的消息和标志位另有 must-ignore 规则兜底。
constexpr uint16_t NegotiateMinor(uint16_t peerMinor) { return std::min(peerMinor, kProtocolMinor); }

// Hello payload 固定 8 字节。这两个字段的唯一用途是与 SO_PEERCRED 的结果交叉比对；
// 会话归属由帧头的 session_id 承担，payload 不再携带任何会话秘密。
//
// 原先这里有一个 nonce，其职责"防止连到其它并发会话"已被两层挡死：抽象地址本身是
// getrandom 生成的唯一随机名，双方都只认自己环境变量里的那一个；SO_PEERCRED 又在两侧
// 把对端 pid 钉死，且来自内核无法伪造。而 session_id 是每帧校验，比 nonce 只在 Hello
// 校验一次覆盖得更早更全，因此 nonce 能挡的场景 session_id 全能挡，反之不成立。
struct HelloPayload {
    uint32_t pid = 0;
    uint32_t uid = 0;
};

// Error payload：8 字节固定头 + 变长 UTF-8 文本。
//
// domain/code 是给机器看的稳定取值，进 CLI 的结构化日志；message 只供人读，接收端
// 不得依赖它的内容做任何分支判断 —— 否则文案一改，判定逻辑就跟着坏。
enum class ErrorDomain : uint16_t {
    INJECTION = 1,     // 注入 / 初始化：环境变量、bind/listen、日志打开
    CONFIGURATION = 2, // 配置：Configure 解码、注册表校验、checker 构造
    PROTOCOL = 3,      // 协议：会话号、序号、长度交叉校验、消息顺序
    INTERNAL = 4,      // 库内部：报告不可用、回调框架异常
};

// 域内错误码，取值仅在所属 domain 内唯一。V1 只区分到"能让日志有稳定取值"的粒度，
// 具体原因由 message 承载。
namespace error_code {
// ErrorDomain::INJECTION
constexpr uint16_t kEnvironmentInvalid = 1;
constexpr uint16_t kListenFailed = 2;
// Code 3 is reserved (removed file-logger initialization failure).
// ErrorDomain::CONFIGURATION
constexpr uint16_t kToolInitializationFailed = 1;
constexpr uint16_t kConfigureMalformed = 2;
// ErrorDomain::PROTOCOL
constexpr uint16_t kFrameRejected = 1;
// ErrorDomain::INTERNAL
constexpr uint16_t kReportUnavailable = 1;
} // namespace error_code

constexpr size_t kErrorHeaderSize = 8;
constexpr size_t kMaxErrorMessageSize = 1024;

struct ErrorPayload {
    ErrorDomain domain = ErrorDomain::INTERNAL;
    uint16_t code = 0;
    std::string message; // UTF-8，不以 NUL 结尾；超长时由编码器截断
};

// ---------------------------------------------------------------------------
// 共享协议定义与选项注册表
//
// 本节是 ToolId / OptionId / OptionValue / ToolRequest 的唯一定义处。CLI 与注入库
// 链接同一 common 目标，两端禁止各自复制一份 ID 或编码表。
// ---------------------------------------------------------------------------

enum class ToolId : uint16_t {
    MEMCHECK = 1,  // 内存访问与生命周期检查
    SYNCCHECK = 2, // 同步原语及 barrier 使用检查
    INITCHECK = 3, // 持久化寄存器首次使用前初始化检查
};

// Configure 中工具子选项的稳定线路标识，由共享注册表映射到名称和值编码。
enum class OptionId : uint16_t {
    MEMCHECK_CHECK_CACHE_CONTROL = 0x0101,
    SYNCCHECK_MISSING_BARRIER_INIT_IS_FATAL = 0x0201,
};

// 以下为内存逻辑模型，与线路布局无关：字段顺序、对齐、padding 都可能与线上不同，
// 编码器必须逐字段读写，不得把结构体整体 memcpy 到帧里。
struct OptionValue {
    OptionId optionId = OptionId::MEMCHECK_CHECK_CACHE_CONTROL; // 唯一确定所属工具和值规则
    std::vector<uint8_t> value;                                 // 注册表编码器生成的规范字节
};

struct ToolRequest {
    ToolId toolId = ToolId::MEMCHECK;
    std::vector<OptionValue> options; // 按 optionId 升序，不重复
};

// Configure 的完整请求。规范化编码唯一：tools 按 toolId 升序不重复，每个工具内 options
// 按 optionId 升序不重复。接收端必须拒绝未排序或重复的编码 —— 只有这样，"子选项换个
// 位置写、编码结果不变"才是可断言的性质。
struct ConfigureRequest {
    uint16_t globalFlags = 0; // V1 必须为 0
    std::vector<ToolRequest> tools;
};

// 工具子选项的注册项。名称在 CLI 中全局唯一，因此子选项可以出现在所属 --tool 之前或
// 之后，解析器不依赖"当前工具"状态，也不通过参数相邻关系推断归属。
struct OptionRegistryEntry {
    const char* name;         // 命令行名称，不含前导 "--"
    OptionId optionId;        // 唯一确定所属工具、值编码与校验规则
    ToolId toolId;            // 所属工具
    uint16_t introducedMinor; // 引入该选项的协议 minor
    uint16_t valueSize;       // V1 均为 1
    uint8_t presentValue;     // "出现即为真"的开关值，V1 均为 0x01
};

// 工具名与 ToolId 的双向查找。未知工具名必须被拒绝，不能静默忽略。
const char* ToolName(ToolId id);
bool LookupTool(const std::string& name, ToolId& id);

// 按命令行名称（不含 "--"）查注册项；未注册时返回 nullptr。
const OptionRegistryEntry* LookupOption(const std::string& name);

// 按 optionId 查注册项，供 Server 解码 Configure 时校验归属、值宽度与值域。
const OptionRegistryEntry* LookupOptionById(OptionId optionId);

// toolId 是否为本实现认识的工具。未知工具必须被拒绝，不能静默忽略。
bool IsKnownTool(uint16_t toolId);

// 全量注册表，供帮助文本和测试遍历。
const std::vector<OptionRegistryEntry>& OptionRegistry();

std::vector<uint8_t> EncodeFrame(const Frame& frame);
bool DecodeFrame(const uint8_t* data, size_t size, Frame& frame, std::string& error);

std::vector<uint8_t> EncodeHello(const HelloPayload& hello);
bool DecodeHello(const std::vector<uint8_t>& payload, HelloPayload& hello, std::string& error);

// 校验请求里的每个选项都落在 negotiatedMinor 的覆盖范围内。
//
// 两端都要查，因为任何一端都可能是较旧的实现：CLI 在**发送 Configure 之前**查，
// 避免把对端无法理解的选项送上线路；Server 解码后再查一遍，因为它不能假设对端守规矩。
bool ValidateConfigureMinor(const ConfigureRequest& request, uint16_t negotiatedMinor, std::string& error);

std::vector<uint8_t> EncodeConfigure(const ConfigureRequest& request);
bool DecodeConfigure(const std::vector<uint8_t>& payload, ConfigureRequest& request, std::string& error);

std::vector<uint8_t> EncodeError(const ErrorPayload& payload);
bool DecodeError(const std::vector<uint8_t>& payload, ErrorPayload& decoded, std::string& error);

std::vector<uint8_t> EncodeText(const std::string& text);
bool DecodeText(const std::vector<uint8_t>& payload, std::string& text, std::string& error);

const char* MessageTypeName(MessageType type);

} // namespace npucheck::ipc

#endif // NPU_TOOLS_NPU_CHECK_SRC_SHARED_WIRE_PROTOCOL_H
