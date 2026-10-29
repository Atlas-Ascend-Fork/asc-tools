/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "options.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace npucheck {
namespace {

bool NeedValue(int argc, char** argv, int& index, std::string& value, std::string& error)
{
    if (index + 1 >= argc) {
        error = std::string("missing value for ") + argv[index];
        return false;
    }
    value = argv[++index];
    return true;
}

// 解析十进制整数并做闭区间值域校验。内部验证选项与对外选项走同一套流程，
// 值域校验同样在解析阶段完成，越界即报用法错误，不留到运行期。
std::optional<int> ParseBoundedInt(const std::string& text, long low, long high)
{
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value < low || value > high) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::string AbsolutePath(const std::string& path)
{
    if (path.empty()) {
        return {};
    }
    boost::system::error_code error;
    auto absolute = boost::filesystem::absolute(path, error);
    return error ? path : absolute.lexically_normal().string();
}

bool IsRegularFile(const boost::filesystem::path& path)
{
    boost::system::error_code error;
    return boost::filesystem::is_regular_file(path, error);
}

// 注入库文件名。CLI 只负责定位它并把规范化绝对路径写进 ACL_API_INJECTION，
// 真正的加载由 ACL Runtime 在目标进程里完成。
constexpr const char* kInjectionLibraryName = "libnpu_check_processor.so";

// CANN 安装根目录。
//
// 必须是 ASCEND_TOOLKIT_HOME —— CANN 的 set_env.sh 导出的是这个名字，并不存在
// ASCEND_TOOLKIT_PATH。用错名字的后果不是"找不到"而是更糟：取到空串后拼接出的
// "/lib64/libnpu_check_processor.so" 是一个宿主机上的绝对路径，查找会静默落到系统目录里去。
constexpr const char* kAscendToolkitHomeEnv = "ASCEND_TOOLKIT_HOME";

// 注入库在 CANN 安装树内的相对目录，由 asc-tools 的 run 包布局决定。
constexpr const char* kInjectionLibraryRelativeDir = "tools/npu_tools/lib64";

// 应用名能否被 execvp 解析到。含 '/' 时按路径直接查，否则沿 PATH 逐段查找 ——
// 必须与 execvp(3) 的查找规则一致，否则这里放行的命令 exec 时仍会失败。
bool IsExecutableCommand(const std::string& command)
{
    const auto executable = [](const boost::filesystem::path& path) {
        boost::system::error_code error;
        return boost::filesystem::is_regular_file(path, error) && access(path.c_str(), X_OK) == 0;
    };
    if (command.find('/') != std::string::npos) {
        return executable(command);
    }
    const char* search = std::getenv("PATH");
    if (search == nullptr || search[0] == '\0') {
        return false;
    }
    const std::string path(search);
    size_t begin = 0;
    while (begin <= path.size()) {
        const size_t end = path.find(':', begin);
        const std::string item = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        // PATH 里的空项按 POSIX 表示当前目录。
        if (executable(boost::filesystem::path(item.empty() ? "." : item) / command)) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return false;
}

// 注入库会被加载进目标进程并以目标进程的权限运行。组可写或其他人可写意味着本用户
// 之外的人能替换它的内容，等于把任意代码执行的入口交出去，因此一律拒绝而不是警告。
bool IsSafelyOwned(const boost::filesystem::path& path, std::string& reason)
{
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        reason = "cannot stat '" + path.string() + "': " + std::strerror(errno);
        return false;
    }
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        reason = "'" + path.string() + "' is group- or world-writable and cannot be injected";
        return false;
    }
    return true;
}

} // namespace

bool ValidateLogFilePath(const std::string& path, std::string& error)
{
    const boost::filesystem::path file(path);
    if (path.empty() || path.back() == '/' || file.filename() == "." || file.filename() == "..") {
        error = "--log-file requires a file path: " + path;
        return false;
    }
    boost::system::error_code ec;
    const auto parent = file.has_parent_path() ? file.parent_path() : boost::filesystem::path(".");
    if (!boost::filesystem::is_directory(parent, ec) || ec) {
        error = "log file parent is not an existing directory: " + parent.string();
        return false;
    }
    const auto status = boost::filesystem::status(file, ec);
    if (ec && ec != boost::system::errc::no_such_file_or_directory) {
        error = "cannot inspect log file: " + path + ": " + ec.message();
        return false;
    }
    if (boost::filesystem::exists(status) && !boost::filesystem::is_regular_file(status)) {
        error = "--log-file requires a file path, not a directory: " + path;
        return false;
    }
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options, std::string& error)
{
    constexpr std::string_view LONG_OPTION_PREFIX = "--";
    options = {};

    // --tool 显式指定的集合。std::set 天然去重（重复指定同一工具即幂等）且按 toolId
    // 升序，正好是下发前要求的规范化顺序，不必再单独排序去重。
    std::set<npucheck::ipc::ToolId> explicitTools;
    // 已出现的工具子选项。std::map 按 optionId 升序，同样直接满足规范化要求；
    // 同一子选项重复出现按幂等处理。
    std::map<npucheck::ipc::OptionId, const npucheck::ipc::OptionRegistryEntry*> seenOptions;
    int applicationStart = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--") {
            applicationStart = i + 1;
            break;
        }
        if (argument == "--help" || argument == "-h") {
            // 记下之后继续扫描，不在这里返回。命令行里可能还有写错的选项，用户需要
            // 同时知道"哪里写错了"和"正确写法是什么"；扫到 -h 就停会让它右侧的错误
            // 被静默吞掉，例如 `-h --tool badtool` 只打帮助、退 0，用户以为写法没问题。
            // 用户敲 -h 往往正因为不确定写法，此刻隐瞒错误代价最大。
            options.showHelp = true;
            continue;
        }
        if (argument.rfind('-', 0) != 0) {
            // 第一个不以 '-' 开头的参数即应用区域起点，其后参数一律原样交给被测程序，
            // 不再由 CLI 解析。因此 "--" 是可选的：写与不写解析出的 Options 完全相同。
            applicationStart = i;
            break;
        }

        std::string value;
        if (argument == "--tool") {
            if (!NeedValue(argc, argv, i, value, error)) {
                return false;
            }
            npucheck::ipc::ToolId toolId{};
            if (!npucheck::ipc::LookupTool(value, toolId)) {
                error =
                    "unknown tool '" + value + "'; supported tools are memcheck, synccheck, initcheck, and racecheck";
                return false;
            }
            explicitTools.insert(toolId);
            continue;
        }
        if (argument == "--log-file") {
            if (!NeedValue(argc, argv, i, value, error)) {
                return false;
            }
            if (!ValidateLogFilePath(value, error)) {
                return false;
            }
            options.logFile = AbsolutePath(value);
            continue;
        }
        if (argument == "--work-dir") {
            if (!NeedValue(argc, argv, i, value, error)) {
                return false;
            }
            // 转绝对路径：这个值要跨 fork 传给注入库，而子进程的当前目录不保证与 CLI
            // 相同，相对路径会在两侧解析到不同位置。
            options.workDir = AbsolutePath(value);
            continue;
        }
        if (argument == "--handshake-timeout-ms") {
            if (!NeedValue(argc, argv, i, value, error)) {
                return false;
            }
            const auto parsed = ParseBoundedInt(
                value, npucheck::ipc::MIN_HANDSHAKE_TIMEOUT_MS, npucheck::ipc::MAX_HANDSHAKE_TIMEOUT_MS);
            if (!parsed) {
                error = "--handshake-timeout-ms must be in [100, 120000]";
                return false;
            }
            options.handshakeTimeoutMs = *parsed;
            continue;
        }
        if (argument == "--error-exitcode") {
            if (!NeedValue(argc, argv, i, value, error)) {
                return false;
            }
            const auto parsed = ParseBoundedInt(value, 1, 255);
            if (!parsed) {
                error = "--error-exitcode must be in [1, 255]";
                return false;
            }
            options.errorExitCode = *parsed;
            continue;
        }
        // 工具子选项：名称在 CLI 中全局唯一，归属由共享注册表决定，因此可以出现在
        // 所属 --tool 之前或之后，这里不依赖"当前工具"状态，也不按参数相邻关系推断。
        if (argument.compare(0, LONG_OPTION_PREFIX.size(), LONG_OPTION_PREFIX) == 0 &&
            argument.size() > LONG_OPTION_PREFIX.size()) {
            if (const auto* entry = npucheck::ipc::LookupOption(argument.substr(LONG_OPTION_PREFIX.size()));
                entry != nullptr) {
                seenOptions[entry->optionId] = entry;
                continue;
            }
        }
        error = "unknown option: " + argument;
        return false;
    }

    // 完全没有出现 --tool 时工具集合取默认值 {memcheck}；只要出现过任意一个 --tool，
    // 默认值即不生效，不与显式指定的工具做并集 —— 否则用户没法把默认工具关掉。
    std::set<npucheck::ipc::ToolId> enabledTools = explicitTools;
    if (enabledTools.empty()) {
        enabledTools.insert(npucheck::ipc::ToolId::MEMCHECK);
    }

    // 子选项的依赖校验必须在默认值生效之后进行：否则只写 --check-cache-control 而不写
    // --tool memcheck 时，会因为此刻工具集合还是空的而被误判为"所属工具未启用"。
    for (const auto& [optionId, entry] : seenOptions) {
        (void)optionId;
        if (enabledTools.count(entry->toolId) == 0) {
            error = std::string("--") + entry->name + " belongs to tool '" + npucheck::ipc::ToolName(entry->toolId) +
                    "', which is not enabled";
            return false;
        }
    }

    // 规范化编码唯一：tools 按 toolId 升序，每个工具内 options 按 optionId 升序，均不重复。
    for (const npucheck::ipc::ToolId toolId : enabledTools) {
        npucheck::ipc::ToolRequest request;
        request.toolId = toolId;
        for (const auto& [optionId, entry] : seenOptions) {
            if (entry->toolId != toolId) {
                continue;
            }
            npucheck::ipc::OptionValue optionValue;
            optionValue.optionId = optionId;
            // 布尔类子选项是"出现即为真"的开关，缺省时不发送 OptionValue。
            optionValue.value.assign(entry->valueSize, entry->presentValue);
            request.options.push_back(std::move(optionValue));
        }
        options.tools.push_back(std::move(request));
    }

    // --help / -h 只打印帮助，不启动任何东西，因此不要求提供应用。这条检查放在选项
    // 校验之后：先把写错的选项报出来，再决定要不要因为"缺应用"而失败。
    if (applicationStart < 0 || applicationStart >= argc) {
        if (options.showHelp) {
            return true;
        }
        error = "expected an application command";
        return false;
    }
    for (int i = applicationStart; i < argc; ++i) {
        options.application.emplace_back(argv[i]);
    }

    // 应用名在 fork 之前就校验，写错时报用法错误 64 而不是等 execvp 失败后退 125。
    //
    // 这条最常见的触发形态是把工具名多写了一遍，例如
    //   npu-check --tool synccheck synccheck --tool memcheck memcheck ./app
    // 多余的 "synccheck" 会成为应用区起点、被当成应用名。此时报"应用不可执行"，
    // 比 fork 之后一句 execvp failed 更接近用户实际写错的地方。
    if (!IsExecutableCommand(options.application.front())) {
        error = "application '" + options.application.front() + "' is not an executable command";
        return false;
    }
    return true;
}

bool ResolveLibraryPath(const std::string& requested, std::string& resolved, std::string& error)
{
    std::vector<boost::filesystem::path> candidates;
    if (!requested.empty()) {
        candidates.emplace_back(requested);
    } else {
        // 注入库只从 CANN 安装树取，不再回退到可执行文件的相邻目录。
        //
        // 这样规定是为了让"注入哪个 so"完全由当前 source 的 CANN 环境决定：相邻目录
        // 兜底会让开发树里的构建产物在装了 CANN 的机器上照样被选中，两者版本不一致时
        // 症状极难定位 —— 注入库和 libacl_san.so / Runtime 必须来自同一套安装（5.3）。
        const char* toolkitHome = std::getenv(kAscendToolkitHomeEnv);
        if (toolkitHome == nullptr || toolkitHome[0] == '\0') {
            error = std::string(kAscendToolkitHomeEnv) + " is not set; source the CANN set_env.sh first";
            return false;
        }
        // 包布局由 asc-tools 的 run 包决定：<CANN>/tools/npu_tools/lib64/。
        // 架构目录这一层必须带上 —— 版本目录下的 tools/ 是另一个真实目录，不含本包产物。
        const boost::filesystem::path root(toolkitHome);
        utsname system{};
        if (uname(&system) == 0 && system.machine[0] != '\0') {
            candidates.push_back(
                root / (std::string(system.machine) + "-linux") / kInjectionLibraryRelativeDir / kInjectionLibraryName);
        }
        // 兼容历史带架构目录层的布局，仍限定在同一个 CANN 根之下。
        candidates.push_back(root / kInjectionLibraryRelativeDir / kInjectionLibraryName);
    }

    // 找到了文件但权限不合格，与"压根没找到"是两种完全不同的故障，诊断必须分开报，
    // 否则用户会一直去检查路径而想不到是文件模式的问题。
    std::string rejection;
    for (const auto& candidate : candidates) {
        boost::system::error_code filesystemError;
        const auto canonical = boost::filesystem::canonical(candidate, filesystemError);
        if (filesystemError || !IsRegularFile(canonical)) {
            continue;
        }
        std::string reason;
        if (!IsSafelyOwned(canonical, reason)) {
            if (rejection.empty()) {
                rejection = reason;
            }
            continue;
        }
        resolved = canonical.string();
        return true;
    }
    if (!rejection.empty()) {
        error = "refusing to inject " + std::string(kInjectionLibraryName) + ": " + rejection;
        return false;
    }
    error = "cannot locate " + std::string(kInjectionLibraryName) + " under " + std::string(kAscendToolkitHomeEnv) +
            "; expected <" + kAscendToolkitHomeEnv + ">/<arch>-linux/" + kInjectionLibraryRelativeDir;
    return false;
}

std::string Usage()
{
    return "Usage: npu-check [--tool <name>]... [--log-file <filepath>]\n"
           "                 [--] <application> [args...]\n"
           "\n"
           "Options:\n"
           "  --tool <name> (=memcheck)\n"
           "      Set the tool to use. Can be specified multiple times to run several tools.\n"
           "        memcheck  : The memory access error detection tool.\n"
           "        synccheck : The thread synchronization hazard detection tool.\n"
           "        initcheck : The persistent register and GM/L1/L0A/L0B/L0C initialization check tool.\n"
           "        racecheck : The memory access race detection tool.\n"
           "  --log-file <filepath>\n"
           "      Save the check report to the file instead of printing output to stdout.\n"
           "  -h, --help\n"
           "      Produce this help message.\n"
           "  --\n"
           "      End the tool options; needed when the application path or its arguments begin "
           "with '-'\n";
}

} // namespace npucheck
