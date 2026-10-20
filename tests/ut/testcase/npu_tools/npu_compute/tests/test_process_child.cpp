/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc < 2) {
        return 2;
    }
    const std::string mode(argv[1]);
    if (mode == "arguments") {
        for (int i = 2; i < argc; ++i) {
            std::printf("%s\n", argv[i]);
        }
        std::fprintf(stderr, "child stderr\n");
        return 0;
    }
    if (mode == "exit") {
        return argc == 3 ? std::atoi(argv[2]) : 2;
    }
    if (mode == "signal") {
        std::raise(SIGTERM);
        return 3;
    }
    if (mode == "environment" && argc == 3) {
        const char* value = std::getenv(argv[2]);
        std::printf("%s", value == nullptr ? "<unset>" : value);
        return 0;
    }
    if (mode == "directory") {
        char* directory = ::getcwd(nullptr, 0);
        if (directory == nullptr) {
            return 1;
        }
        std::printf("%s", directory);
        std::free(directory);
        return 0;
    }
    if (mode == "large-output") {
        const std::string block(4096, 'x');
        for (int i = 0; i < 64; ++i) {
            if (std::fwrite(block.data(), 1, block.size(), stdout) != block.size() ||
                std::fwrite(block.data(), 1, block.size(), stderr) != block.size()) {
                return 1;
            }
        }
        return 0;
    }
    if (mode == "sleep" || mode == "descendant") {
        if (mode == "descendant") {
            const pid_t child = ::fork();
            if (child < 0) {
                return 1;
            }
            if (child > 0) {
                std::printf("%ld", static_cast<long>(child));
                std::fflush(stdout);
            }
        }
        for (;;) {
            ::pause();
        }
    }
    return 2;
}
