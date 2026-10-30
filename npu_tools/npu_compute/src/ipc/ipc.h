/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_IPC_IPC_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_IPC_IPC_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sys/types.h>

namespace npucompute::ipc {

using Bytes = std::vector<uint8_t>;
using Deadline = std::chrono::steady_clock::time_point;
inline constexpr uint32_t kMaxValueBytes = 64 * 1024;
inline constexpr size_t kChunkBytes = 48 * 1024;
inline constexpr size_t kHeaderBytes = 12;
inline constexpr size_t kPrefixBytes = 22;
inline constexpr size_t kMaxFrameBytes = kHeaderBytes + kMaxValueBytes;
inline constexpr std::chrono::milliseconds kHandshakeTimeout{10000};
inline constexpr std::chrono::milliseconds kIoTimeout{30000};
inline constexpr std::chrono::milliseconds kDrainTimeout{30000};
inline constexpr std::chrono::milliseconds kResultAckTimeout{30000};
inline constexpr uint32_t kCacheControlModeAll = 0;
inline constexpr uint32_t kCacheControlModeNone = 1;
enum class Type : uint16_t {
    Connect = 1,
    Interrupt = 2,
    ConnectAck = 3,
    Flush = 4,
    Result = 5,
    ResultAck = 6,
    Error = 7,
    Status = 8,
    Diagnostic = 9,
    DataBegin = 0x100,
    Pmu = 0x101,
    Pipeline = 0x102,
    Hardware = 0x103,
    DataEnd = 0x104,
    PmSampling = 0x105
};
enum class DataKind : uint16_t { PmuCsv = 1, PipelineTrace = 2, HardwareInfo = 3, PmSampling = 4 };
enum class ArtifactType : uint16_t { Csv = 1, Jsonl = 2, Json = 3, Db = 4, Txt = 5 };
struct Config {
    std::vector<std::string> sections;
    std::string replayMode = "kernel";
    uint64_t featureFlags = 0;
    uint32_t cacheControlMode = kCacheControlModeNone;
};
struct Hello {
    uint32_t pid = 0;
    uint32_t uid = 0;
    Config config;
};
struct Ready {
    uint32_t pid = 0;
    uint32_t uid = 0;
    uint32_t status = 0;
};
struct ArtifactBegin {
    uint64_t id = 0;
    DataKind kind = DataKind::PmuCsv;
    std::string name;
    ArtifactType artifactType = ArtifactType::Csv;
};
struct ArtifactChunk {
    uint64_t id = 0;
    uint64_t offset = 0;
    Bytes data;
};
struct ArtifactEnd {
    uint64_t id = 0;
    uint64_t bytes = 0;
    uint64_t records = 0;
};
struct ManifestEntry {
    uint64_t id = 0;
    std::string name;
    uint64_t bytes = 0;
};
struct Result {
    uint32_t status = 0;
    uint32_t code = 0;
    bool complete = true;
    bool degraded = false;
    uint64_t replayCount = 0;
    uint64_t rowCount = 0;
    uint64_t droppedCount = 0;
    uint64_t errorCount = 0;
    std::vector<std::string> sections;
    std::vector<ManifestEntry> manifest;
};
struct Error {
    uint32_t code = 0;
    std::string message;
    std::string domain;
    std::string phase;
};
class Exception : public std::runtime_error {
public:
    explicit Exception(Error error) : std::runtime_error(error.message), error_(std::move(error)) {}
    const Error& Detail() const { return error_; }

private:
    Error error_;
};
struct Bootstrap {
    std::string name;
    uint64_t session = 0;
    pid_t cliPid = 0;
    int timeoutMs = static_cast<int>(kHandshakeTimeout.count());
    Deadline handshakeDeadline;
};
struct Frame {
    Type type;
    Bytes payload;
};

Deadline After(std::chrono::milliseconds timeout);
Bootstrap MakeBootstrap();
void ValidateConfig(const Config& config);
DataKind KindForName(const std::string& name);
ArtifactType ArtifactTypeForName(const std::string& name);
Type TypeForKind(DataKind kind);
Bytes EncodeConfig(const Config& value);
Config DecodeConfig(const Bytes& value);
Bytes EncodeHello(const Hello& value);
Hello DecodeHello(const Bytes& value);
Bytes EncodeReady(const Ready& value);
Ready DecodeReady(const Bytes& value);
Bytes EncodeArtifactBegin(const ArtifactBegin& value);
ArtifactBegin DecodeArtifactBegin(const Bytes& value);
Bytes EncodeArtifactChunk(const ArtifactChunk& value);
ArtifactChunk DecodeArtifactChunk(const Bytes& value);
Bytes EncodeArtifactEnd(const ArtifactEnd& value);
ArtifactEnd DecodeArtifactEnd(const Bytes& value);
Bytes EncodeResult(const Result& value);
Result DecodeResult(const Bytes& value);
Bytes EncodeError(const Error& value);
Error DecodeError(const Bytes& value);

// All operations throw std::runtime_error on protocol, transport, or deadline errors.
// A channel is used by one thread at a time; ownership may be transferred by move.
class Channel {
public:
    Channel() = default;
    ~Channel();
    Channel(Channel&& other) noexcept;
    Channel& operator=(Channel&& other) noexcept;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    static Channel Connect(
        const std::string& name, uint64_t session, pid_t peerPid, uid_t peerUid, Deadline deadline,
        const std::function<bool()>& cancelled = {}, bool allowDescendants = false);
    void Send(Type type, const Bytes& payload, Deadline deadline, const std::function<bool()>& cancelled = {});
    Frame Receive(Deadline deadline, const std::function<bool()>& cancelled = {});
    int Fd() const { return fd_; }
    pid_t PeerPid() const;
    void Close() noexcept;
    void Shutdown() const noexcept;

private:
    friend class Listener;
    Channel(int fd, uint64_t session) : fd_(fd), session_(session) {}
    int fd_ = -1;
    uint64_t session_ = 0;
    uint64_t sendSequence_ = 0;
    uint64_t receiveSequence_ = 0;
};

class Listener {
public:
    explicit Listener(const std::string& name);
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Channel Accept(uint64_t session, pid_t peerPid, uid_t peerUid, Deadline deadline);
    int Fd() const { return fd_; }

private:
    int fd_ = -1;
};

} // namespace npucompute::ipc
#endif // NPU_TOOLS_NPU_COMPUTE_SRC_IPC_IPC_H
