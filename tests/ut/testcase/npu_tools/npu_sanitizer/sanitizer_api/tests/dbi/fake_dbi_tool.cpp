// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::set<std::string> ExtractReportSymbols(const std::string& input)
{
    constexpr std::string_view kPrefix = "__sanitizer_report_";
    std::set<std::string> symbols;
    for (size_t begin = input.find(kPrefix); begin != std::string::npos; begin = input.find(kPrefix, begin)) {
        size_t end = begin + kPrefix.size();
        while (end < input.size() && (std::isalnum(static_cast<unsigned char>(input[end])) || input[end] == '_')) {
            ++end;
        }
        symbols.insert(input.substr(begin, end - begin));
        begin = end;
    }
    return symbols;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
        return 2;
    }
    const std::string tool = std::filesystem::path(argv[0]).filename().string();
    const char* logPath = std::getenv("DBI_FAKE_LOG");
    if (logPath == nullptr || logPath[0] == '\0') {
        return 2;
    }
    std::ofstream log(logPath, std::ios::app);
    if (!log) {
        return 2;
    }
    log << tool;
    for (int index = 1; index < argc; ++index) {
        log << " <" << argv[index] << '>';
    }
    log << '\n';

    const char* failTool = std::getenv("DBI_FAKE_FAIL");
    if (failTool != nullptr && tool == failTool) {
        return 7;
    }

    if (tool == "llvm-objdump") {
        const std::string input = argc > 1 ? argv[argc - 1] : "";
        std::ifstream artifact(input, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(artifact), std::istreambuf_iterator<char>()};
        const std::set<std::string> symbols = ExtractReportSymbols(contents);
        if (symbols.empty()) {
            constexpr std::string_view prefix = ".ascend.meta.";
            std::set<std::string> kernels;
            for (size_t pos = contents.find(prefix); pos != std::string::npos;
                 pos = contents.find(prefix, pos + prefix.size())) {
                const size_t begin = pos + prefix.size();
                const size_t end = contents.find('\0', begin);
                if (end != std::string::npos && end > begin) {
                    kernels.insert(contents.substr(begin, end - begin));
                }
            }
            if (kernels.empty()) {
                kernels.insert("FullFlowKernel");
            }
            for (const auto& kernel : kernels) {
                std::cout << "00000000 g F .text.kernel 00000010 " << kernel << '\n';
            }
        } else {
            for (const std::string& symbol : symbols) {
                std::cout << "00000000 w F .text.probe 00000010 " << symbol << '\n';
            }
        }
        return 0;
    }

    std::string output;
    std::vector<std::string> inputs;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-o" && index + 1 < argc) {
            output = argv[++index];
        } else if (argument.rfind("-o=", 0) == 0) {
            output = argument.substr(3);
        } else if (std::filesystem::is_regular_file(argument)) {
            inputs.push_back(argument);
        }
    }
    if (output.empty()) {
        return 2;
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(output).parent_path(), error);
    if (error) {
        return 2;
    }
    std::ofstream artifact(output, std::ios::binary | std::ios::trunc);
    // 链接和插桩阶段保留输入 ELF，使后续元数据校验使用真实 ELF 布局。
    for (const std::string& input : inputs) {
        std::ifstream source(input, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        if ((tool == "ld.lld" || tool == "bisheng-tune") && contents.compare(0, 4, "\177ELF") == 0) {
            artifact << contents;
            return artifact.good() ? 0 : 2;
        }
    }
    artifact << "fake-" << tool << '\n';
    for (const std::string& input : inputs) {
        std::ifstream source(input, std::ios::binary);
        artifact << source.rdbuf();
    }
    return artifact.good() ? 0 : 2;
}
