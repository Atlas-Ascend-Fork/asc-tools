/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "compute/ipc/artifact_publisher.h"
#include "compute/ipc/npu_compute_uds_server.h"
#include "pm_sampling/pm_sampling_sqlite_writer.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr std::string_view kHardwareInfo = "{\"category\":\"Host Info\",\"cpu physical count\":1}\n"
                                           "{\"category\":\"Device Info\",\"npu count\":1}\n"
                                           "{\"category\":\"CPU Information\",\"control cpu count\":1}\n"
                                           "{\"category\":\"AI Core Information\",\"ai core count\":1}\n"
                                           "{\"category\":\"Memory Information\",\"hbm total(MB)\":1}\n";
constexpr std::string_view kOccupancy =
    R"({"status":"complete","traceEvents":[{"ph":"X","name":"Task","pid":1,"tid":0,"ts":0,"dur":1}]})";
constexpr std::string_view kPipeCsv = "block_id,pipe_utilization\n0,75\n";
constexpr std::string_view kMemoryCsv = "block_id,read_bytes\n0,128\n";
constexpr std::string_view kL2CacheCsv = "block_id,hit_rate\n0,99\n";
constexpr std::string_view kSummary = "{\"category\":\"PipeUtilization\"}\n{\"category\":\"OpInfoSummary\"}\n";
constexpr std::string_view kSamplingSummary =
    "{\"category\":\"PerformanceHints\",\"hints\":[]}\n{\"category\":\"OpInfoSummary\"}\n";

std::string SamplingDatabase()
{
    struct TemporaryFile {
        char path[64] = "/tmp/npu-compute-sampling-fixture-XXXXXX";
        ~TemporaryFile() { ::unlink(path); }
    } temporary;
    const int descriptor = ::mkstemp(temporary.path);
    if (descriptor < 0) {
        throw std::runtime_error("cannot create sampling fixture");
    }
    ::close(descriptor);
    npucompute::PmSamplingResult sampling;
    sampling.rows[0x268] = {0, 215, 16320, 0, 0, 0, 0, 0};
    std::string error;
    if (!npucompute::WritePmSamplingSqlite(temporary.path, sampling, error)) {
        throw std::runtime_error(error);
    }
    std::ifstream input(temporary.path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || bytes.empty()) {
        throw std::runtime_error("cannot read sampling fixture");
    }
    return bytes;
}

int PublishApplicationRank(bool abortAfterArtifact)
{
    namespace ipc = npucompute::ipc;
    if (std::getenv("NPU_COMPUTE_REPLAY_MODE") == nullptr) {
        throw std::runtime_error("Application transport mode is missing");
    }
    const auto deadline = ipc::After(ipc::kHandshakeTimeout);
    auto channel = ipc::Channel::Connect(
        std::getenv("NPU_COMPUTE_UDS_NAME"), std::stoull(std::getenv("NPU_COMPUTE_SESSION_ID")),
        static_cast<pid_t>(std::stol(std::getenv("NPU_COMPUTE_CLI_PID"))), getuid(), deadline);
    const auto frame = channel.Receive(deadline);
    if (frame.type != ipc::Type::Connect) {
        throw std::runtime_error("Expected CONNECT");
    }
    const auto config = ipc::DecodeHello(frame.payload).config;
    if (config.replayMode != "application") {
        throw std::runtime_error("Expected Application replay");
    }
    if (abortAfterArtifact) {
        channel.Send(
            ipc::Type::ConnectAck,
            ipc::EncodeReady({static_cast<uint32_t>(getpid()), static_cast<uint32_t>(getuid()), 0}), deadline);
        channel.Send(
            ipc::Type::DataBegin,
            ipc::EncodeArtifactBegin({1, ipc::DataKind::HardwareInfo, "HardwareInfo.jsonl", ipc::ArtifactType::Jsonl}),
            deadline);
        channel.Send(
            ipc::Type::Hardware,
            ipc::EncodeArtifactChunk({1, 0, ipc::Bytes(kHardwareInfo.begin(), kHardwareInfo.end())}), deadline);
        channel.Send(ipc::Type::DataEnd, ipc::EncodeArtifactEnd({1, kHardwareInfo.size(), 5}), deadline);
        channel.Send(
            ipc::Type::DataBegin,
            ipc::EncodeArtifactBegin(
                {2, ipc::DataKind::Application, "application_manifest.json", ipc::ArtifactType::Json}),
            deadline);
        return 23;
    }
    npucompute::ArtifactPublisher publisher(std::move(channel), deadline);
    const auto publish = [&publisher](const std::string& name, std::string_view bytes) {
        publisher.Begin(name);
        publisher.Write(bytes);
        publisher.Commit(1);
    };
    publish("HardwareInfo.jsonl", kHardwareInfo);
    publish("application_raw.jsonl", "{\"recordType\":\"metadata\"}\n");
    publish("application_manifest.json", "{\"complete\":true}\n");
    ipc::Result result;
    result.sections = config.sections;
    result.complete = true;
    return publisher.Finish(std::move(result)) ? 0 : 3;
}

int RunApplicationRanks(const std::string& mode)
{
    if (mode == "application-single") {
        return PublishApplicationRank(false);
    }
    std::vector<pid_t> ranks;
    for (int index = 0; index < 2; ++index) {
        const pid_t rank = fork();
        if (rank == 0) {
            try {
                _exit(PublishApplicationRank(mode == "application-abort" && index == 1));
            } catch (const std::exception& error) {
                std::fprintf(stderr, "[rep-fixture] %s\n", error.what());
                _exit(2);
            }
        }
        if (rank < 0) {
            throw std::runtime_error("Cannot fork fixture rank");
        }
        ranks.push_back(rank);
    }
    int status = 0;
    for (const auto rank : ranks) {
        int childStatus = 0;
        if (waitpid(rank, &childStatus, 0) != rank || !WIFEXITED(childStatus)) {
            return 2;
        }
        if (WEXITSTATUS(childStatus) != 0) {
            status = WEXITSTATUS(childStatus);
        }
    }
    return status;
}

bool ParseExitCode(const char* value, int* exit_code)
{
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == nullptr || end[0] != '\0' || parsed < 0 || parsed > 255) {
        return false;
    }
    *exit_code = static_cast<int>(parsed);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    int exit_code = 0;
    std::string mode;
    std::vector<std::string> applicationArguments;
    for (int index = 1; index < argc; ++index) {
        if ((mode == "cli-echo" || mode == "exec" || mode == "protocol-bad-offset") &&
            std::strcmp(argv[index], "--") == 0) {
            while (++index < argc) {
                applicationArguments.emplace_back(argv[index]);
            }
            break;
        }
        if (std::strcmp(argv[index], "--mode") == 0 && index + 1 < argc) {
            mode = argv[++index];
            continue;
        }
        if (std::strcmp(argv[index], "--exit-code") != 0 || index + 1 >= argc ||
            !ParseExitCode(argv[++index], &exit_code)) {
            std::fprintf(stderr, "[rep-fixture] invalid arguments\n");
            return 2;
        }
    }

    try {
        if (mode.rfind("application-", 0) == 0) {
            return RunApplicationRanks(mode);
        }
        for (const char* legacy : {"NPU_COMPUTE_REPLAY_MODE"}) {
            if (std::getenv(legacy) != nullptr) {
                throw std::runtime_error(std::string("legacy configuration leaked: ") + legacy);
            }
        }
        npucompute::UdsServer server;
        const auto hello = server.Accept();
        if (mode == "cli-echo") {
            std::printf("sections=");
            for (std::size_t index = 0; index < hello.config.sections.size(); ++index) {
                std::printf("%s%s", index == 0 ? "" : ",", hello.config.sections[index].c_str());
            }
            std::printf("\nreplay=%s\n", hello.config.replayMode.c_str());
            for (const auto& argument : applicationArguments) {
                std::printf("argument=");
                for (unsigned char byte : argument) {
                    std::printf("%02x", static_cast<unsigned>(byte));
                }
                std::printf("\n");
            }
        }
        if (mode == "protocol-bad-offset") {
            namespace ipc = npucompute::ipc;
            auto channel = server.TakeChannel();
            const auto deadline = server.Deadline();
            channel.Send(
                ipc::Type::ConnectAck,
                ipc::EncodeReady({static_cast<uint32_t>(getpid()), static_cast<uint32_t>(getuid()), 0}), deadline);
            channel.Send(
                ipc::Type::DataBegin,
                ipc::EncodeArtifactBegin(
                    {1, ipc::DataKind::HardwareInfo, "HardwareInfo.jsonl", ipc::ArtifactType::Jsonl}),
                deadline);
            channel.Send(
                ipc::Type::Hardware,
                ipc::EncodeArtifactChunk({1, 0, ipc::Bytes(kHardwareInfo.begin(), kHardwareInfo.end())}), deadline);
            channel.Send(ipc::Type::DataEnd, ipc::EncodeArtifactEnd({1, kHardwareInfo.size(), 5}), deadline);
            channel.Send(
                ipc::Type::DataBegin,
                ipc::EncodeArtifactBegin({2, ipc::DataKind::PmuCsv, "Memory.csv", ipc::ArtifactType::Csv}), deadline);
            channel.Send(ipc::Type::Pmu, ipc::EncodeArtifactChunk({2, 1, {'x'}}), deadline);
            const auto response = channel.Receive(deadline);
            if (response.type != ipc::Type::Error) {
                throw std::runtime_error("expected protocol rejection");
            }
            std::fprintf(stderr, "[rep-fixture] %s\n", ipc::DecodeError(response.payload).message.c_str());
            if (!applicationArguments.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                std::ofstream marker(applicationArguments.front());
                marker << "closed\n";
                marker.close();
                if (!marker) {
                    return 3;
                }
            }
            return 0;
        }
        npucompute::ArtifactPublisher publisher(server.TakeChannel(), server.Deadline());
        const auto publish = [&](const std::string& name, std::string_view bytes) {
            publisher.Begin(name);
            publisher.Write(bytes);
            publisher.Commit(1);
        };
        if (mode != "empty") {
            if (mode != "missing-occupancy") {
                if (mode == "empty-occupancy") {
                    publisher.PublishOptional("Occupancy.json", {}, 0);
                } else {
                    publisher.PublishOptional("Occupancy.json", kOccupancy, 1);
                }
            }
            if (mode != "missing-hardware") {
                publish("HardwareInfo.jsonl", kHardwareInfo);
            }
            for (const auto& section : hello.config.sections) {
                if (section == "Roofline" || section == "SourceCounter") {
                    continue;
                }
                if (section == "Pipeline") {
                    if (mode == "cli-echo" || mode == "missing-warp" || mode.rfind("pm-sampling", 0) == 0) {
                        publish(
                            ".biu-staging/process-uds/manifest.json",
                            R"({"version":1,"state":"complete","status":0,"fragments":[{"resultSequence":"0","replayId":"0","deviceId":0,"file":"result-0.json","eventCount":"1"}]})");
                        publish(
                            ".biu-staging/process-uds/result-0.json",
                            R"({"displayTimeUnit":"ns","profilingType":"op","schemaVersion":1,"traceEvents":[{"cname":"startup","dur":0.5,"name":"SCALAR","ph":"X","pid":"group0.cubecore","tid":"SCALAR","ts":1.0}]})");
                    }
                    continue;
                }
                if (section == "PmSampling" && (mode.rfind("pm-sampling", 0) == 0 || mode == "cli-echo")) {
                    if (mode == "pm-sampling-simd") {
                        publish("objdump.txt", "Disassembly of section .text:\n0: 0000000000000000 VF X2, #9\n");
                        continue;
                    }
                    if (mode != "pm-sampling-missing-all") {
                        if (mode != "pm-sampling-missing-db") {
                            publish(
                                "summary.db",
                                mode == "pm-sampling-invalid-db" ? "broken database" : SamplingDatabase());
                        }
                        if (mode != "pm-sampling-missing-objdump") {
                            publish(
                                "objdump.txt", mode == "pm-sampling-invalid-objdump" ?
                                                   "no instructions" :
                                                   "Disassembly of section .text:\n268: 00 00 STG\n");
                        }
                        if (mode != "pm-sampling-missing-symbolizer") {
                            publish(
                                "symbolizer.json", mode == "pm-sampling-invalid-symbolizer" ?
                                                       "broken JSON" :
                                                       "[{\"Address\":\"0x0000000000000268\",\"Symbol\":[]}]\n");
                        }
                    }
                    continue;
                }
                if (mode == "csv-directories" && section == "Memory") {
                    publish("collection-p1-0001/Memory.csv", "block_id,read_bytes\n1,256\n");
                    publish("collection-p1-0001/PipeUtilization.csv", kPipeCsv);
                    publish("collection-p1-0001/summary.jsonl", kSummary);
                    publisher.PublishOptional("collection-p1-0001/Occupancy.json", kOccupancy, 1);
                }
                if (section == "Roofline") {
                    continue; // Roofline is carried in the summary below.
                }
                if (section == "occupancy") {
                    if (mode != "missing-warp") {
                        publish(npucompute::ipc::kSimtOccupancyFileName, R"({"traceEvents":[]})");
                    }
                    continue;
                }
                publish(
                    section + ".csv",
                    mode == "invalid-csv" ? "header\n" :
                    (mode == "csv-field-count" || mode == "pm-sampling-csv-field-count") && section == "Memory" ?
                                            "block_id,read_bytes\n0\n" :
                    mode == "csv-unit-conflict" && section == "Memory" ? "block_id,latency(us)\n0,2\n" :
                    mode == "csv-unit-conflict"                        ? "block_id,latency(ms)\n0,1\n" :
                    section == "PipeUtilization"                       ? kPipeCsv :
                    section == "L2Cache"                               ? kL2CacheCsv :
                                                                         kMemoryCsv);
            }
            if (mode == "performance-hints" || mode == "empty-hints" || mode == "invalid-hints" ||
                mode == "chunked-hints" || mode == "hints-before-exit") {
                const std::string hint = "[PH03][AIV] aiv_mte2_ratio（MTE2 流水占比）为 82%，超过 50% 且占比最高。";
                std::string hints =
                    mode == "empty-hints" ?
                        "[]" :
                    mode == "invalid-hints" ?
                        "[1]" :
                        "[\"" + hint + (mode == "chunked-hints" ? std::string(200000, 'x') : "") + "\"]";
                publish(
                    "summary.jsonl",
                    "{\"category\":\"PipeUtilization\"}\n"
                    "{\"category\":\"PerformanceHints\",\"hints\":" +
                        hints +
                        "}\n"
                        "{\"category\":\"OpInfoSummary\",\"Op Name\":\"fixture_kernel\",\"Device Id\":0}\n");
                if (mode == "hints-before-exit") {
                    ::usleep(500000);
                    std::puts("fixture-exiting");
                    std::fflush(stdout);
                }
            } else if (mode.rfind("pm-sampling", 0) == 0 && mode != "pm-sampling-missing-summary") {
                publish("summary.jsonl", kSamplingSummary);
            } else if (mode != "pm-sampling-missing-summary") {
                publish("summary.jsonl", kSummary);
            }
        }
        npucompute::ipc::Result result;
        result.sections = hello.config.sections;
        result.rowCount = mode == "empty" ? 0 : 1;
        result.complete = mode != "incomplete";
        result.status = result.code = result.complete ? 0 : 1;
        if (!publisher.Finish(std::move(result)) && exit_code == 0) {
            return 3;
        }
        if (mode == "exec" && !applicationArguments.empty()) {
            std::vector<char*> arguments;
            for (auto& argument : applicationArguments) {
                arguments.push_back(argument.data());
            }
            arguments.push_back(nullptr);
            ::execvp(arguments.front(), arguments.data());
            std::perror("[rep-fixture] exec application failed");
            return 127;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[rep-fixture] %s\n", error.what());
        return 2;
    }
    std::fprintf(stderr, "[rep-fixture] artifacts published\n");
    return exit_code;
}
