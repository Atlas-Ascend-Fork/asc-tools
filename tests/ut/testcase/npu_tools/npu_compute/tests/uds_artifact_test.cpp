/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "cli/ipc/artifact_receiver.h"
#include "compute/ipc/artifact_publisher.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <thread>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace npucompute::ipc;
using npucompute::ArtifactPublisher;
using npucompute::cli::ArtifactReceiver;
namespace fs = std::filesystem;

void Check(bool condition)
{
    if (!condition) {
        throw std::runtime_error("UDS artifact test check failed");
    }
}
void Reject(const std::function<void()>& operation)
{
    bool failed = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        failed = true;
    }
    Check(failed);
}
class TempDirectory {
public:
    TempDirectory()
    {
        char path[] = "/tmp/npu-compute-artifact-test-XXXXXX";
        const auto created = mkdtemp(path);
        Check(created != nullptr);
        path_ = created;
    }
    ~TempDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    const std::string& Path() const { return path_; }

private:
    std::string path_;
};
std::string ReadFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Check(input.is_open());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
void Begin(ArtifactReceiver& receiver, uint64_t id, const std::string& name)
{
    const auto kind = KindForName(name);
    receiver.Receive({Type::DataBegin, EncodeArtifactBegin({id, kind, name, ArtifactTypeForName(name)})});
}
void Chunk(ArtifactReceiver& receiver, uint64_t id, uint64_t offset, const std::string& data)
{
    receiver.Receive({Type::Pmu, EncodeArtifactChunk({id, offset, Bytes(data.begin(), data.end())})});
}
void HardwareChunk(ArtifactReceiver& receiver, uint64_t id, const std::string& data)
{
    receiver.Receive(
        {TypeForKind(DataKind::HardwareInfo), EncodeArtifactChunk({id, 0, Bytes(data.begin(), data.end())})});
}
void TypedChunk(ArtifactReceiver& receiver, uint64_t id, DataKind kind, const std::string& data)
{
    receiver.Receive({TypeForKind(kind), EncodeArtifactChunk({id, 0, Bytes(data.begin(), data.end())})});
}
void End(ArtifactReceiver& receiver, uint64_t id, uint64_t length)
{
    receiver.Receive({Type::DataEnd, EncodeArtifactEnd({id, length, 1})});
}
void TestRoundTripAndManifest()
{
    TempDirectory temporary;
    ArtifactReceiver receiver(temporary.Path());
    const std::string data("a,b\n1,2\n\0tail", 13);
    Begin(receiver, 1, "MemoryL0.csv");
    Chunk(receiver, 1, 0, data.substr(0, 3));
    Chunk(receiver, 1, 3, data.substr(3));
    End(receiver, 1, data.size());
    Check(ReadFile(fs::path(temporary.Path()) / "MemoryL0.csv") == data);
    Begin(receiver, 2, "HardwareInfo.jsonl");
    HardwareChunk(receiver, 2, "{}\n");
    End(receiver, 2, 3);
    Begin(receiver, 3, "summary.jsonl");
    TypedChunk(receiver, 3, DataKind::PmuCsv, "{}\n");
    End(receiver, 3, 3);
    Begin(receiver, 4, ".biu-staging/process-uds/manifest.json");
    TypedChunk(receiver, 4, DataKind::PipelineTrace, "{}\n");
    End(receiver, 4, 3);
    Begin(receiver, 5, ".biu-staging/process-uds/result-0-device-0-replay-0.json");
    TypedChunk(receiver, 5, DataKind::PipelineTrace, "{}\n");
    End(receiver, 5, 3);
    Config config;
    config.sections = {"MemoryL0", "Pipeline"};
    Result result;
    result.sections = config.sections;
    result.manifest = {
        {1, "MemoryL0.csv", data.size()},
        {2, "HardwareInfo.jsonl", 3},
        {3, "summary.jsonl", 3},
        {4, ".biu-staging/process-uds/manifest.json", 3},
        {5, ".biu-staging/process-uds/result-0-device-0-replay-0.json", 3},
    };
    receiver.Validate(result, config);
    auto broken = result;
    broken.manifest[0].bytes++;
    Reject([&] { receiver.Validate(broken, config); });
    broken = result;
    broken.manifest.pop_back();
    Reject([&] { receiver.Validate(broken, config); });
    broken = result;
    broken.manifest[1] = broken.manifest[0];
    Reject([&] { receiver.Validate(broken, config); });
    broken = result;
    broken.complete = false;
    broken.status = broken.code = 1;
    Check(!receiver.Validate(broken, config));
    broken = result;
    broken.droppedCount = 1;
    Check(!receiver.Validate(broken, config));
    Reject([&] { Begin(receiver, 3, "MemoryL0.csv"); });
    Reject([&] { Begin(receiver, 1, "Memory.csv"); });
}
void TestMalformedAndCleanup()
{
    TempDirectory temporary;
    {
        ArtifactReceiver receiver(temporary.Path());
        auto payload = EncodeArtifactBegin({1, DataKind::PmuCsv, "MemoryL0.csv"});
        payload.back() = '/';
        Reject([&] { receiver.Receive({Type::DataBegin, payload}); });
        Reject([&] { Chunk(receiver, 1, 0, "abc"); });
        Begin(receiver, 1, "MemoryL0.csv");
        Reject([&] { Chunk(receiver, 1, 1, "abc"); });
        Check(fs::is_empty(temporary.Path()));
        Begin(receiver, 1, "MemoryL0.csv");
        Chunk(receiver, 1, 0, "abc");
        Reject([&] { End(receiver, 1, 4); });
        Check(fs::is_empty(temporary.Path()));
        Begin(receiver, 1, "MemoryL0.csv");
        Reject([&] { Chunk(receiver, 2, 0, "abc"); });
        Begin(receiver, 1, "MemoryL0.csv");
        Reject([&] { Begin(receiver, 2, "Memory.csv"); });
        Check(fs::is_empty(temporary.Path()));
        Begin(receiver, 1, "MemoryL0.csv");
        Chunk(receiver, 1, 0, "unfinished");
    }
    Check(fs::is_empty(temporary.Path()));
}
void TestNoOverwrite()
{
    TempDirectory temporary;
    const fs::path root(temporary.Path());
    {
        std::ofstream sentinel(root / "sentinel");
        sentinel << "preserve";
    }
    fs::create_symlink(root / "sentinel", root / "MemoryL0.csv");
    {
        ArtifactReceiver receiver(temporary.Path());
        Begin(receiver, 1, "MemoryL0.csv");
        Chunk(receiver, 1, 0, "changed");
        Reject([&] { End(receiver, 1, 7); });
    }
    Check(ReadFile(root / "sentinel") == "preserve");
    Check(fs::is_symlink(root / "MemoryL0.csv"));
    Check(!fs::exists(root / ".MemoryL0.csv.1.tmp"));
    fs::remove(root / "MemoryL0.csv");
    {
        std::ofstream existing(root / "MemoryL0.csv");
        existing << "existing";
    }
    {
        ArtifactReceiver receiver(temporary.Path());
        Begin(receiver, 1, "MemoryL0.csv");
        Chunk(receiver, 1, 0, "changed");
        Reject([&] { End(receiver, 1, 7); });
    }
    Check(ReadFile(root / "MemoryL0.csv") == "existing");
    fs::create_directory_symlink(root, root / "alias");
    Reject([&] { ArtifactReceiver receiver((root / "alias").string()); });
    fs::create_symlink(root / "sentinel", root / ".Memory.csv.2.tmp");
    {
        ArtifactReceiver receiver(temporary.Path());
        Reject([&] { Begin(receiver, 2, "Memory.csv"); });
    }
    Check(ReadFile(root / "sentinel") == "preserve");
    Check(fs::is_symlink(root / ".Memory.csv.2.tmp"));
}
void TestPublisherProcess()
{
    TempDirectory temporary;
    std::string data(2 * 1024 * 1024, '\0');
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(i % 251);
    }
    const auto bootstrap = MakeBootstrap();
    const pid_t child = fork();
    Check(child >= 0);
    if (child == 0) {
        alarm(15);
        try {
            Listener listener(bootstrap.name);
            ArtifactPublisher publisher(
                listener.Accept(bootstrap.session, getppid(), getuid(), bootstrap.handshakeDeadline));
            publisher.Begin("MemoryL0.csv");
            publisher.Write(data);
            publisher.Commit(1234);
            publisher.Begin("HardwareInfo.jsonl");
            publisher.Write("{}\n");
            publisher.Commit(1);
            publisher.Begin("summary.jsonl");
            publisher.Write("{}\n");
            publisher.Commit(1);
            Result result;
            result.sections = {"MemoryL0"};
            result.rowCount = 1234;
            _exit(publisher.Finish(result) ? 0 : 11);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "publisher child: %s\n", error.what());
            _exit(12);
        }
    }
    try {
        auto client = Channel::Connect(bootstrap.name, bootstrap.session, child, getuid(), bootstrap.handshakeDeadline);
        ArtifactReceiver receiver(temporary.Path());
        Config config;
        config.sections = {"MemoryL0"};
        for (;;) {
            const auto frame = client.Receive(After(std::chrono::seconds(10)));
            if (frame.type == Type::Result) {
                const auto result = DecodeResult(frame.payload);
                Check(result.rowCount == 1234);
                receiver.Validate(result, config);
                Check(ReadFile(fs::path(temporary.Path()) / "MemoryL0.csv") == data);
                Check(ReadFile(fs::path(temporary.Path()) / "HardwareInfo.jsonl") == "{}\n");
                Check(ReadFile(fs::path(temporary.Path()) / "summary.jsonl") == "{}\n");
                client.Send(Type::ResultAck, {}, After(std::chrono::seconds(1)));
                break;
            }
            receiver.Receive(frame);
        }
        int status = 0;
        Check(waitpid(child, &status, 0) == child);
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    } catch (...) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        throw;
    }
}
void TestPublisherControl(Type control, bool expectedSuccess, bool finalReply = false)
{
    const auto bootstrap = MakeBootstrap();
    const pid_t child = fork();
    Check(child >= 0);
    if (child == 0) {
        alarm(10);
        try {
            Listener listener(bootstrap.name);
            ArtifactPublisher publisher(
                listener.Accept(bootstrap.session, getppid(), getuid(), bootstrap.handshakeDeadline));
            publisher.Begin("Memory.csv");
            publisher.Write("header\nvalue\n");
            publisher.Commit(1);
            // Allow an early control to reach the worker before requesting finalization.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Result result;
            result.sections = {"Memory"};
            const bool success = publisher.Finish(result);
            _exit(success == expectedSuccess ? 0 : 11);
        } catch (...) {
            _exit(12);
        }
    }
    try {
        auto client = Channel::Connect(bootstrap.name, bootstrap.session, child, getuid(), bootstrap.handshakeDeadline);
        const Bytes payload =
            control == Type::Error ? EncodeError({9, "receiver rejected data", "ARTIFACT", "receive"}) : Bytes{};
        if (!finalReply) {
            client.Send(control, payload, After(std::chrono::seconds(1)));
        }
        bool terminal = false;
        for (;;) {
            const auto frame = client.Receive(After(std::chrono::seconds(3)));
            if (frame.type == Type::Error) {
                Check(!expectedSuccess);
                Check(!DecodeError(frame.payload).message.empty());
                terminal = true;
                break;
            }
            if (frame.type == Type::Result) {
                if (finalReply) {
                    client.Send(control, payload, After(std::chrono::seconds(1)));
                } else {
                    Check(expectedSuccess);
                    client.Send(Type::ResultAck, {}, After(std::chrono::seconds(1)));
                    terminal = true;
                    break;
                }
            }
        }
        Check(terminal);
        int status = 0;
        Check(waitpid(child, &status, 0) == child);
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    } catch (...) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        throw;
    }
}
} // namespace
int main()
{
    try {
        TestRoundTripAndManifest();
        TestMalformedAndCleanup();
        TestNoOverwrite();
        TestPublisherProcess();
        TestPublisherControl(Type::Flush, true);
        TestPublisherControl(Type::Interrupt, false);
        TestPublisherControl(Type::Error, false);
        TestPublisherControl(Type::ConnectAck, false, true);
        std::puts("UDS artifact tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
