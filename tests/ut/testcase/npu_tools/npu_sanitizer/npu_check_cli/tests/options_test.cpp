// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "options.h"

#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace npucheck {
namespace {

struct ParseResult {
    bool ok = false;
    Options options{};
    std::string error;
};

ParseResult Parse(std::vector<std::string> arguments)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }

    ParseResult result{};
    result.ok = ParseOptions(static_cast<int>(argv.size()), argv.data(), result.options, result.error);
    return result;
}

// 解析阶段会校验应用名能否被 execvp 解析到，所以凡是期望解析成功的用例都必须给一个
// 真实存在且可执行的路径，不能再用 kSampleApp 这类占位串。/bin/sh 由 POSIX 保证存在，
// 比每个用例现造临时文件更省事，也不引入清理逻辑。
constexpr const char* kSampleApp = "/bin/sh";

class TemporaryFile {
public:
    TemporaryFile()
    {
        char pattern[] = "/tmp/npu_check_cli_test_XXXXXX";
        const int fd = mkstemp(pattern);
        EXPECT_NE(fd, -1);
        if (fd >= 0) {
            (void)close(fd);
            path_ = pattern;
        }
    }

    ~TemporaryFile()
    {
        if (!path_.empty()) {
            boost::system::error_code error;
            boost::filesystem::remove(path_, error);
        }
    }

    const boost::filesystem::path& Path() const { return path_; }

private:
    boost::filesystem::path path_;
};

TEST(OptionsTest, AcceptsHelpWithoutApplication)
{
    for (const char* flag : {"--help", "-h"}) {
        const auto result = Parse({"npu_check", flag});

        ASSERT_TRUE(result.ok) << flag << ": " << result.error;
        EXPECT_TRUE(result.options.showHelp) << flag;
        EXPECT_TRUE(result.options.application.empty()) << flag;
    }
}

// -h 不中断扫描：它右侧的选项照常解析，写错了就报错，不会被帮助信息掩盖。
// 用户敲 -h 往往正因为不确定写法，此刻隐瞒错误代价最大 —— `-h --tool badtool`
// 若只打帮助并退 0，用户会以为自己的写法没问题。
TEST(OptionsTest, HelpDoesNotSuppressErrorsOnItsRight)
{
    const auto afterHelp = Parse({"npu_check", "-h", "--tool", "unknown"});
    EXPECT_FALSE(afterHelp.ok);
    EXPECT_EQ(
        afterHelp.error, "unknown tool 'unknown'; supported tools are memcheck, synccheck, initcheck, and racecheck");

    const auto unknownOption = Parse({"npu_check", "-h", "--nonsense"});
    EXPECT_FALSE(unknownOption.ok);
    EXPECT_EQ(unknownOption.error, "unknown option: --nonsense");

    // 反向顺序早就会报错，这里一并锁住：错误与 -h 的相对位置无关。
    const auto beforeHelp = Parse({"npu_check", "--tool", "unknown", "-h"});
    EXPECT_FALSE(beforeHelp.ok);
    EXPECT_EQ(
        beforeHelp.error, "unknown tool 'unknown'; supported tools are memcheck, synccheck, initcheck, and racecheck");
}

// 选项本身合法时 -h 仍然只是"打印帮助"，不因为缺少应用而失败。
TEST(OptionsTest, HelpWithValidOptionsSucceeds)
{
    const auto result = Parse({"npu_check", "-h", "--tool", "memcheck"});

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.options.showHelp);
    EXPECT_TRUE(result.options.application.empty());
}

// -h 与应用同时出现时两者都保留：由调用方决定先打帮助还是直接跑。
TEST(OptionsTest, HelpCoexistsWithApplication)
{
    const auto result = Parse({"npu_check", "-h", kSampleApp});

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.options.showHelp);
    EXPECT_EQ(result.options.application, (std::vector<std::string>{kSampleApp}));
}

// 把 Options::tools 压成 (toolId, [optionId...]) 便于断言顺序与去重。
std::vector<std::pair<uint16_t, std::vector<uint16_t>>> ToolShape(const Options& options)
{
    std::vector<std::pair<uint16_t, std::vector<uint16_t>>> shape;
    for (const auto& tool : options.tools) {
        std::vector<uint16_t> optionIds;
        for (const auto& option : tool.options) {
            optionIds.push_back(static_cast<uint16_t>(option.optionId));
        }
        shape.emplace_back(static_cast<uint16_t>(tool.toolId), std::move(optionIds));
    }
    return shape;
}

constexpr uint16_t kMemcheck = static_cast<uint16_t>(npucheck::ipc::ToolId::MEMCHECK);
constexpr uint16_t kSynccheck = static_cast<uint16_t>(npucheck::ipc::ToolId::SYNCCHECK);
constexpr uint16_t kInitcheck = static_cast<uint16_t>(npucheck::ipc::ToolId::INITCHECK);
constexpr uint16_t kRacecheck = static_cast<uint16_t>(npucheck::ipc::ToolId::RACECHECK);
constexpr uint16_t kCheckCacheControl = static_cast<uint16_t>(npucheck::ipc::OptionId::MEMCHECK_CHECK_CACHE_CONTROL);
constexpr uint16_t kMissingBarrierInitIsFatal =
    static_cast<uint16_t>(npucheck::ipc::OptionId::SYNCCHECK_MISSING_BARRIER_INIT_IS_FATAL);

// 完全没有 --tool 时工具集合取默认值 {memcheck}。
TEST(OptionsTest, DefaultsToMemcheckWhenNoToolGiven)
{
    const auto result = Parse({"npu_check", kSampleApp});

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(ToolShape(result.options), (decltype(ToolShape(result.options)){{kMemcheck, {}}}));
    EXPECT_EQ(result.options.application, (std::vector<std::string>{kSampleApp}));
}

// "--" 是可选的：带与不带解析为同一 Options。
TEST(OptionsTest, SeparatorIsOptional)
{
    const auto withSeparator = Parse({"npu_check", "--tool", "memcheck", "--", kSampleApp, "--size", "64"});
    const auto withoutSeparator = Parse({"npu_check", "--tool", "memcheck", kSampleApp, "--size", "64"});

    ASSERT_TRUE(withSeparator.ok) << withSeparator.error;
    ASSERT_TRUE(withoutSeparator.ok) << withoutSeparator.error;
    EXPECT_EQ(withSeparator.options.application, withoutSeparator.options.application);
    EXPECT_EQ(ToolShape(withSeparator.options), ToolShape(withoutSeparator.options));
    // app_name 之后的参数不再由 CLI 解析。
    EXPECT_EQ(withoutSeparator.options.application, (std::vector<std::string>{kSampleApp, "--size", "64"}));
}

// 只要出现过任意一个 --tool，默认值即不生效，不与显式指定的工具做并集。
TEST(OptionsTest, ExplicitToolSuppressesDefault)
{
    const auto result = Parse({"npu_check", "--tool", "synccheck", kSampleApp});

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(ToolShape(result.options), (decltype(ToolShape(result.options)){{kSynccheck, {}}}));
}

TEST(OptionsTest, AcceptsInitcheckAndThreeToolComposition)
{
    const auto initcheck = Parse({"npu_check", "--tool", "initcheck", kSampleApp});
    ASSERT_TRUE(initcheck.ok) << initcheck.error;
    EXPECT_EQ(ToolShape(initcheck.options), (decltype(ToolShape(initcheck.options)){{kInitcheck, {}}}));

    const auto all =
        Parse({"npu_check", "--tool", "initcheck", "--tool", "synccheck", "--tool", "memcheck", kSampleApp});
    ASSERT_TRUE(all.ok) << all.error;
    EXPECT_EQ(
        ToolShape(all.options),
        (decltype(ToolShape(all.options)){{kMemcheck, {}}, {kSynccheck, {}}, {kInitcheck, {}}}));
}

TEST(OptionsTest, RepeatedToolIsIdempotent)
{
    const auto result = Parse({"npu_check", "--tool", "memcheck", "--tool", "memcheck", kSampleApp});

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(ToolShape(result.options), (decltype(ToolShape(result.options)){{kMemcheck, {}}}));
}

TEST(OptionsTest, AcceptsOnlyRacecheckWithoutEnablingDefaultTool)
{
    const auto result = Parse({"npu-check", "--tool", "racecheck", kSampleApp});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(ToolShape(result.options), (decltype(ToolShape(result.options)){{kRacecheck, {}}}));
    const auto rejected = Parse({"npu-check", "--tool", "race-check", kSampleApp});
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(
        rejected.error, "unknown tool 'race-check'; supported tools are memcheck, synccheck, initcheck, and racecheck");
}

TEST(OptionsTest, RepeatedRacecheckDeduplicatesAndSortsWithExistingTools)
{
    const auto result = Parse(
        {"npu-check", "--tool", "racecheck", "--tool", "initcheck", "--tool", "racecheck", "--tool", "synccheck",
         "--tool", "memcheck", kSampleApp});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(
        ToolShape(result.options),
        (decltype(ToolShape(result.options)){{kMemcheck, {}}, {kSynccheck, {}}, {kInitcheck, {}}, {kRacecheck, {}}}));
    npucheck::ipc::ConfigureRequest request;
    request.tools = result.options.tools;
    npucheck::ipc::ConfigureRequest decoded;
    std::string error;
    ASSERT_TRUE(npucheck::ipc::DecodeConfigure(npucheck::ipc::EncodeConfigure(request), decoded, error)) << error;
    ASSERT_EQ(decoded.tools.size(), 4U);
    EXPECT_EQ(static_cast<uint16_t>(decoded.tools.back().toolId), kRacecheck);
}

TEST(OptionsTest, RacecheckRejectsOtherToolsOptionsUnlessTheirOwnerIsEnabled)
{
    for (const auto& entry : npucheck::ipc::OptionRegistry()) {
        const auto result = Parse({"npu-check", "--tool", "racecheck", std::string("--") + entry.name, kSampleApp});
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(
            result.error, std::string("--") + entry.name + " belongs to tool '" +
                              npucheck::ipc::ToolName(entry.toolId) + "', which is not enabled");
    }
    const auto combined =
        Parse({"npu-check", "--tool", "racecheck", "--tool", "memcheck", "--check-cache-control", kSampleApp});
    ASSERT_TRUE(combined.ok) << combined.error;
    EXPECT_EQ(
        ToolShape(combined.options),
        (decltype(ToolShape(combined.options)){{kMemcheck, {kCheckCacheControl}}, {kRacecheck, {}}}));
}

// 规范化编码唯一：tools 按 toolId 升序，与命令行出现顺序无关。
TEST(OptionsTest, ToolsAreSortedByToolId)
{
    const auto reversed = Parse({"npu_check", "--tool", "synccheck", "--tool", "memcheck", kSampleApp});
    const auto ordered = Parse({"npu_check", "--tool", "memcheck", "--tool", "synccheck", kSampleApp});

    ASSERT_TRUE(reversed.ok) << reversed.error;
    ASSERT_TRUE(ordered.ok) << ordered.error;
    EXPECT_EQ(ToolShape(reversed.options), (decltype(ToolShape(reversed.options)){{kMemcheck, {}}, {kSynccheck, {}}}));
    EXPECT_EQ(ToolShape(reversed.options), ToolShape(ordered.options));
}

// 子选项归属由注册表决定，可出现在所属 --tool 之前或之后。
TEST(OptionsTest, SuboptionOwnershipIsIndependentOfPosition)
{
    const auto before = Parse({"npu_check", "--check-cache-control", "--tool", "memcheck", kSampleApp});
    const auto after = Parse({"npu_check", "--tool", "memcheck", "--check-cache-control", kSampleApp});

    ASSERT_TRUE(before.ok) << before.error;
    ASSERT_TRUE(after.ok) << after.error;
    EXPECT_EQ(ToolShape(before.options), (decltype(ToolShape(before.options)){{kMemcheck, {kCheckCacheControl}}}));
    EXPECT_EQ(ToolShape(before.options), ToolShape(after.options));
    ASSERT_EQ(before.options.tools.size(), 1U);
    ASSERT_EQ(before.options.tools.front().options.size(), 1U);
    // 布尔类子选项"出现即为真"，value_size=1 且只取 0x01。
    EXPECT_EQ(before.options.tools.front().options.front().value, (std::vector<uint8_t>{0x01}));

    // 光比对解析结果不够：规范化只做了一半时，解析结果可能相同而编码不同。
    // 编码字节逐一相等才是"位置无关"的真正断言。
    npucheck::ipc::ConfigureRequest beforeRequest;
    beforeRequest.tools = before.options.tools;
    npucheck::ipc::ConfigureRequest afterRequest;
    afterRequest.tools = after.options.tools;
    EXPECT_EQ(npucheck::ipc::EncodeConfigure(beforeRequest), npucheck::ipc::EncodeConfigure(afterRequest));
}

// 依赖校验在默认值生效之后进行：默认集合含 memcheck，故该子选项合法。
TEST(OptionsTest, SuboptionOfDefaultToolIsAccepted)
{
    const auto result = Parse({"npu_check", "--check-cache-control", kSampleApp});

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(ToolShape(result.options), (decltype(ToolShape(result.options)){{kMemcheck, {kCheckCacheControl}}}));
}

TEST(OptionsTest, RejectsSuboptionWhoseToolIsNotEnabled)
{
    // 默认集合不含 synccheck。
    const auto defaultSet = Parse({"npu_check", "--missing-barrier-init-is-fatal", kSampleApp});
    EXPECT_FALSE(defaultSet.ok);
    EXPECT_EQ(defaultSet.error, "--missing-barrier-init-is-fatal belongs to tool 'synccheck', which is not enabled");

    // 显式指定 synccheck 后默认值不生效，memcheck 未启用。
    const auto explicitSet = Parse({"npu_check", "--tool", "synccheck", "--check-cache-control", kSampleApp});
    EXPECT_FALSE(explicitSet.ok);
    EXPECT_EQ(explicitSet.error, "--check-cache-control belongs to tool 'memcheck', which is not enabled");

    // 同时启用两个工具时两个子选项都合法，且各自归属正确。
    const auto both = Parse(
        {"npu_check", "--tool", "memcheck", "--tool", "synccheck", "--check-cache-control",
         "--missing-barrier-init-is-fatal", kSampleApp});
    ASSERT_TRUE(both.ok) << both.error;
    EXPECT_EQ(
        ToolShape(both.options), (decltype(ToolShape(both.options)){
                                     {kMemcheck, {kCheckCacheControl}}, {kSynccheck, {kMissingBarrierInitIsFatal}}}));
}

TEST(OptionsTest, RejectsInvalidInvocation)
{
    const auto missingApplication = Parse({"npu_check", "--tool", "memcheck"});
    EXPECT_FALSE(missingApplication.ok);
    EXPECT_EQ(missingApplication.error, "expected an application command");

    const auto danglingSeparator = Parse({"npu_check", "--tool", "memcheck", "--"});
    EXPECT_FALSE(danglingSeparator.ok);
    EXPECT_EQ(danglingSeparator.error, "expected an application command");

    const auto unknownTool = Parse({"npu_check", "--tool", "trace", "--", kSampleApp});
    EXPECT_FALSE(unknownTool.ok);
    EXPECT_EQ(
        unknownTool.error, "unknown tool 'trace'; supported tools are memcheck, synccheck, initcheck, and racecheck");

    const auto missingValue = Parse({"npu_check", "--tool"});
    EXPECT_FALSE(missingValue.ok);
    EXPECT_EQ(missingValue.error, "missing value for --tool");

    const auto unknownOption = Parse({"npu_check", "--unknown", "--", kSampleApp});
    EXPECT_FALSE(unknownOption.ok);
    EXPECT_EQ(unknownOption.error, "unknown option: --unknown");

    const auto shortUnknown = Parse({"npu_check", "-x", "--", kSampleApp});
    EXPECT_FALSE(shortUnknown.ok);
    EXPECT_EQ(shortUnknown.error, "unknown option: -x");

    const auto bareDash = Parse({"npu_check", "-", "--", kSampleApp});
    EXPECT_FALSE(bareDash.ok);
    EXPECT_EQ(bareDash.error, "unknown option: -");
}

// 应用名在 fork 之前就校验，写错时报用法错误而不是等 execvp 失败。最常见的触发形态
// 是把工具名多写了一遍（--tool synccheck synccheck ./app），多余的那个会成为应用区
// 起点被当成应用名，此时报"应用不可执行"比 fork 之后一句 execvp failed 更接近现场。
TEST(OptionsTest, RejectsApplicationThatCannotBeExecuted)
{
    const auto duplicatedToolName = Parse({"npu_check", "--tool", "synccheck", "synccheck", kSampleApp});
    EXPECT_FALSE(duplicatedToolName.ok);
    EXPECT_EQ(duplicatedToolName.error, "application 'synccheck' is not an executable command");

    const auto missingPath = Parse({"npu_check", "--", "/tmp/npu_check_missing_application"});
    EXPECT_FALSE(missingPath.ok);
    EXPECT_EQ(missingPath.error, "application '/tmp/npu_check_missing_application' is not an executable command");

    // 存在但没有执行位的文件同样不可执行，不能因为 stat 成功就放行。
    TemporaryFile notExecutable;
    ASSERT_FALSE(notExecutable.Path().empty());
    ASSERT_EQ(chmod(notExecutable.Path().c_str(), 0644), 0);
    const auto denied = Parse({"npu_check", "--", notExecutable.Path().string()});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error, "application '" + notExecutable.Path().string() + "' is not an executable command");

    // 不含 '/' 的名字沿 PATH 查找，与 execvp(3) 一致：sh 必然能找到。
    const auto onPath = Parse({"npu_check", "--", "sh"});
    EXPECT_TRUE(onPath.ok) << onPath.error;
}

// 内部验证选项与对外选项走同一套解析和校验流程，值域校验在解析阶段完成。
TEST(OptionsTest, ValidatesInternalOptionRanges)
{
    EXPECT_FALSE(Parse({"npu_check", "--handshake-timeout-ms", "99", kSampleApp}).ok);
    EXPECT_FALSE(Parse({"npu_check", "--handshake-timeout-ms", "120001", kSampleApp}).ok);
    EXPECT_FALSE(Parse({"npu_check", "--handshake-timeout-ms", "abc", kSampleApp}).ok);

    const auto low = Parse({"npu_check", "--handshake-timeout-ms", "100", kSampleApp});
    ASSERT_TRUE(low.ok) << low.error;
    EXPECT_EQ(low.options.handshakeTimeoutMs, 100);

    const auto high = Parse({"npu_check", "--handshake-timeout-ms", "120000", kSampleApp});
    ASSERT_TRUE(high.ok) << high.error;
    EXPECT_EQ(high.options.handshakeTimeoutMs, 120000);

    EXPECT_FALSE(Parse({"npu_check", "--error-exitcode", "0", kSampleApp}).ok);
    EXPECT_FALSE(Parse({"npu_check", "--error-exitcode", "256", kSampleApp}).ok);

    const auto exitLow = Parse({"npu_check", "--error-exitcode", "1", kSampleApp});
    ASSERT_TRUE(exitLow.ok) << exitLow.error;
    EXPECT_EQ(exitLow.options.errorExitCode, 1);

    const auto exitHigh = Parse({"npu_check", "--error-exitcode", "255", kSampleApp});
    ASSERT_TRUE(exitHigh.ok) << exitHigh.error;
    EXPECT_EQ(exitHigh.options.errorExitCode, 255);

    // 未指定时为 0，表示不覆盖应用退出码。
    EXPECT_EQ(Parse({"npu_check", kSampleApp}).options.errorExitCode, 0);
}

TEST(OptionsTest, NormalizesLogFileToAbsolutePath)
{
    const auto result = Parse({"npu_check", "--log-file", "report.log", kSampleApp});

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(boost::filesystem::path(result.options.logFile).is_absolute());
    EXPECT_TRUE(Parse({"npu_check", kSampleApp}).options.logFile.empty());
}

TEST(OptionsTest, LogFileRequiresAnExistingParentAndAFileName)
{
    const auto root = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("log-path-%%%%-%%%%");
    boost::filesystem::create_directories(root);
    for (const auto& path :
         {root.string(), root.string() + "/", (root / "missing/").string(), (root / "missing/report.log").string(),
          std::string{}, std::string("/dev/null")}) {
        EXPECT_FALSE(Parse({"npu-check", "--log-file", path, kSampleApp}).ok) << path;
    }
    EXPECT_TRUE(Parse({"npu-check", "--log-file", (root / "report").string(), kSampleApp}).ok);
    TemporaryFile existing;
    EXPECT_TRUE(Parse({"npu-check", "--log-file", existing.Path().string(), kSampleApp}).ok);
    boost::filesystem::remove_all(root);
}

// --work-dir 的值要跨 fork 传给注入库，而子进程的当前目录不保证与 CLI 相同，
// 相对路径会在两侧解析到不同位置，因此必须在解析阶段就转成绝对路径。
TEST(OptionsTest, NormalizesWorkDirToAbsolutePath)
{
    const auto result = Parse({"npu_check", "--work-dir", "probe_runtime", kSampleApp});

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(boost::filesystem::path(result.options.workDir).is_absolute());
    EXPECT_EQ(boost::filesystem::path(result.options.workDir).filename().string(), "probe_runtime");

    // 未指定时为空，由 CLI 退回临时会话目录。
    EXPECT_TRUE(Parse({"npu_check", kSampleApp}).options.workDir.empty());

    const auto missingValue = Parse({"npu_check", "--work-dir"});
    EXPECT_FALSE(missingValue.ok);
    EXPECT_EQ(missingValue.error, "missing value for --work-dir");
}

// 这三个选项随选项注册表改造一并移除，语义已由子选项或库侧默认值承担，
// 不能因为旧脚本还在传就悄悄接受它们。
TEST(OptionsTest, RejectsRemovedLegacyOptions)
{
    for (const char* removed : {"--strict", "--keep-temp", "--probe-cache-dir"}) {
        const auto result = Parse({"npu_check", removed, kSampleApp});
        EXPECT_FALSE(result.ok) << removed;
        EXPECT_EQ(result.error, std::string("unknown option: ") + removed);
    }
}

TEST(OptionsTest, ResolvesExplicitRegularFile)
{
    TemporaryFile library;
    ASSERT_FALSE(library.Path().empty());

    std::string resolved;
    std::string error;
    ASSERT_TRUE(ResolveLibraryPath(library.Path().string(), resolved, error)) << error;

    EXPECT_EQ(resolved, boost::filesystem::canonical(library.Path()).string());
}

TEST(OptionsTest, RejectsMissingLibrary)
{
    std::string resolved;
    std::string error;

    EXPECT_FALSE(ResolveLibraryPath("/tmp/npu_check_missing_library.so", resolved, error));
    // 诊断必须点出实际搜索过的位置，而不是只说"没找到"。
    EXPECT_NE(error.find("cannot locate libnpu_check_processor.so"), std::string::npos) << error;
    EXPECT_NE(error.find("ASCEND_TOOLKIT_HOME"), std::string::npos) << error;
}

// 注入库会被加载进目标进程并以其权限运行；组可写或其他人可写意味着别人能替换它的
// 内容，等于交出任意代码执行的入口，因此必须拒绝而不是警告。
TEST(OptionsTest, RejectsGroupOrWorldWritableLibrary)
{
    for (const mode_t mode : {static_cast<mode_t>(0664), static_cast<mode_t>(0646)}) {
        TemporaryFile library;
        ASSERT_FALSE(library.Path().empty());
        ASSERT_EQ(chmod(library.Path().c_str(), mode), 0);

        std::string resolved;
        std::string error;
        EXPECT_FALSE(ResolveLibraryPath(library.Path().string(), resolved, error)) << std::oct << mode;
        // 与"压根没找到"必须是两种不同的诊断，否则用户只会反复去查路径。
        EXPECT_NE(error.find("group- or world-writable"), std::string::npos) << error;
    }
}

TEST(OptionsTest, AcceptsLibraryWithSafePermissions)
{
    TemporaryFile library;
    ASSERT_FALSE(library.Path().empty());
    ASSERT_EQ(chmod(library.Path().c_str(), 0755), 0);

    std::string resolved;
    std::string error;
    ASSERT_TRUE(ResolveLibraryPath(library.Path().string(), resolved, error)) << error;
    EXPECT_EQ(resolved, boost::filesystem::canonical(library.Path()).string());
}

// --help 输出只含对外选项，内部验证选项不得外泄。
TEST(OptionsTest, UsageListsOnlyPublicOptions)
{
    const std::string usage = Usage();

    EXPECT_NE(usage.find("Usage: npu-check"), std::string::npos);
    EXPECT_EQ(usage.find("Usage: npu_check"), std::string::npos);
    // usage 行保留本工具原有的展开写法；Options 段沿用 compute-sanitizer 的排版。
    EXPECT_NE(usage.find("[--tool <name>]..."), std::string::npos);
    EXPECT_NE(usage.find("Options:"), std::string::npos);
    EXPECT_NE(usage.find("--tool"), std::string::npos);
    EXPECT_EQ(usage.find("race-check"), std::string::npos);
    EXPECT_NE(usage.find("racecheck"), std::string::npos);
    EXPECT_EQ(usage.find("--tools"), std::string::npos);
    EXPECT_NE(usage.find("--log-file"), std::string::npos);
    EXPECT_NE(usage.find("--help"), std::string::npos);
    EXPECT_NE(usage.find("-h"), std::string::npos);

    // 内部调测选项照常解析，但不得出现在帮助里 —— 印进去用户就会当成对外契约来依赖。
    EXPECT_EQ(usage.find("--work-dir"), std::string::npos);
    EXPECT_EQ(usage.find("--handshake-timeout-ms"), std::string::npos);
    EXPECT_EQ(usage.find("--error-exitcode"), std::string::npos);
    // 工具子选项尚未对外支持，同样不出现在帮助里。
    EXPECT_EQ(usage.find("--check-cache-control"), std::string::npos);
    EXPECT_EQ(usage.find("--missing-barrier-init-is-fatal"), std::string::npos);
}

TEST(OptionsTest, RejectsRemovedToolsOption)
{
    const auto result = Parse({"npu-check", "--tools", "memcheck", kSampleApp});

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "unknown option: --tools");
}

TEST(OptionsTest, ForwardsRemovedToolsOptionAfterSeparator)
{
    const auto forwarded = Parse({"npu-check", "--tool", "memcheck", "--", kSampleApp, "--tools", "synccheck"});

    ASSERT_TRUE(forwarded.ok) << forwarded.error;
    EXPECT_EQ(forwarded.options.application, (std::vector<std::string>{kSampleApp, "--tools", "synccheck"}));
    EXPECT_EQ(forwarded.options.tools.size(), 1U);
}

} // namespace
} // namespace npucheck
