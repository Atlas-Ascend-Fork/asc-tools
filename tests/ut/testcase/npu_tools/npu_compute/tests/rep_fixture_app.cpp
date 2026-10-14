/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "compute/ipc/artifact_publisher.h"
#include "compute/ipc/npu_compute_uds_server.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace {

constexpr std::string_view kHardwareInfo = "{\"category\":\"Host Info\",\"cpu physical count\":1}\n"
                                           "{\"category\":\"Device Info\",\"npu count\":1}\n"
                                           "{\"category\":\"CPU Information\",\"control cpu count\":1}\n"
                                           "{\"category\":\"AI Core Information\",\"ai core count\":1}\n"
                                           "{\"category\":\"Memory Information\",\"hbm total(MB)\":1}\n";
constexpr std::string_view kPipeCsv = "block_id,pipe_utilization\n0,75\n";
constexpr std::string_view kMemoryCsv = "block_id,read_bytes\n0,128\n";
constexpr std::string_view kL2CacheCsv = "block_id,hit_rate\n0,99\n";
constexpr std::string_view kSummary = "{\"category\":\"PipeUtilization\"}\n{\"category\":\"OpInfoSummary\"}\n";

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
        if ((mode == "cli-echo" || mode == "exec") && std::strcmp(argv[index], "--") == 0) {
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
        npucompute::ArtifactPublisher publisher(server.TakeChannel(), server.Deadline());
        const auto publish = [&](const std::string& name, std::string_view bytes) {
            publisher.Begin(name);
            publisher.Write(bytes);
            publisher.Commit(1);
        };
        if (mode != "empty") {
            if (mode != "missing-hardware") {
                publish("HardwareInfo.jsonl", kHardwareInfo);
            }
            for (const auto& section : hello.config.sections) {
                if (section == "Pipeline") {
                    if (mode == "cli-echo") {
                        publish(
                            ".biu-staging/process-uds/manifest.json",
                            R"({"version":1,"state":"complete","status":0,"fragments":[{"resultSequence":"0","replayId":"0","deviceId":0,"file":"result-0.json","eventCount":"1"}]})");
                        publish(
                            ".biu-staging/process-uds/result-0.json",
                            R"({"displayTimeUnit":"ns","profilingType":"op","schemaVersion":1,"traceEvents":[{"cname":"startup","dur":0.5,"name":"SCALAR","ph":"X","pid":"group0.cubecore","tid":"SCALAR","ts":1.0}]})");
                    }
                    continue;
                }
                publish(
                    section + ".csv", mode == "invalid-csv"        ? "header\n" :
                                      section == "PipeUtilization" ? kPipeCsv :
                                      section == "L2Cache"         ? kL2CacheCsv :
                                                                     kMemoryCsv);
            }
            publish("summary.jsonl", kSummary);
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
