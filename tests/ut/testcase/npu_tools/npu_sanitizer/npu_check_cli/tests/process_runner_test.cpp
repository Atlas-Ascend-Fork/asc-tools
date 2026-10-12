// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "process_runner.h"
#include "../../common/tests/plog_capture.h"
#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <csignal>
#include <fstream>
#include <sstream>

namespace npucheck {
TEST(ProcessRunnerTest, UnavailableChildStatusIsNotReportedAsAnApplicationExitCode)
{
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    struct sigaction previous {};
    ASSERT_EQ(sigaction(SIGCHLD, &ignore, &previous), 0);
    struct RestoreSignal {
        struct sigaction previous;
        ~RestoreSignal() { (void)sigaction(SIGCHLD, &previous, nullptr); }
    } restore{previous};

    Options options;
    options.application = {"/bin/true"};
    options.handshakeTimeoutMs = 100;
    PlogCapture capture;
    EXPECT_EQ(RunApplication(options, "/missing/libnpu_check_processor.so"), INFRASTRUCTURE_ERROR_EXIT_CODE);
    EXPECT_NE(capture.Text().find("waitpid:"), std::string::npos);
    EXPECT_NE(capture.Text().find("child_exit=unknown"), std::string::npos);
}

TEST(ProcessRunnerTest, InternalDiagnosticsUsePlogWithoutContaminatingCheckOutput)
{
    const auto directory =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("npu-check-plog-%%%%-%%%%");
    boost::filesystem::create_directories(directory);
    Options options;
    options.application = {"/bin/true"};
    options.handshakeTimeoutMs = 100;
    options.workDir = directory.string();
    options.logFile = (directory / "check.log").string();
    {
        std::ofstream previous(options.logFile);
        previous << "OLD_REPORT_MUST_BE_REPLACED\n";
    }
    PlogCapture capture;
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const int result = RunApplication(options, "/missing/libnpu_check_processor.so");
    const auto console = testing::internal::GetCapturedStdout() + testing::internal::GetCapturedStderr();
    std::ifstream file(options.logFile);
    std::ostringstream contents;
    contents << file.rdbuf();
    EXPECT_EQ(result, 125); // /bin/true does not initialize ACL or perform the handshake.
    // 机器格式的摘要行只落 plog，不进终端也不进报告文件；终端上拿到的是人类可读提示。
    EXPECT_NE(capture.Text().find("[CLI] outcome=infra_failed"), std::string::npos);
    EXPECT_NE(console.find("tool execution failed"), std::string::npos);
    // infra_failed 的具体原因（握手缺失等）归因不可靠，同样不打屏：只落 plog 与报告文件。
    EXPECT_EQ(console.find("handshake=missing"), std::string::npos);
    EXPECT_NE(capture.Text().find("handshake=missing"), std::string::npos);
    EXPECT_NE(contents.str().find("handshake=missing"), std::string::npos);
    EXPECT_EQ(contents.str().find("OLD_REPORT_MUST_BE_REPLACED"), std::string::npos);
    for (const auto* internal : {"[INJECTION]", "[UDS]", "[CLI] session=", "[CLI] outcome="}) {
        EXPECT_EQ(console.find(internal), std::string::npos);
        EXPECT_EQ(contents.str().find(internal), std::string::npos);
        EXPECT_NE(capture.Text().find(internal), std::string::npos);
    }
    EXPECT_FALSE(boost::filesystem::exists(directory / "npu_check.log"));
    boost::filesystem::remove_all(directory);
}
TEST(ProcessRunnerTest, ApplicationOutputStaysOutOfLogFile)
{
    const auto directory =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("npu-check-console-%%%%-%%%%");
    boost::filesystem::create_directories(directory);
    Options options;
    options.application = {"/bin/echo", "NPU_CHECK_APP_OUTPUT_MARKER"};
    options.handshakeTimeoutMs = 100;
    options.workDir = directory.string();
    options.logFile = (directory / "check.log").string();
    PlogCapture capture;
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const int result = RunApplication(options, "/missing/libnpu_check_processor.so");
    const auto console = testing::internal::GetCapturedStdout() + testing::internal::GetCapturedStderr();
    std::ifstream file(options.logFile);
    std::ostringstream contents;
    contents << file.rdbuf();
    EXPECT_EQ(result, 125); // /bin/echo does not initialize ACL or perform the handshake.
    // --log-file 只收工具自身的输出：应用打印仅转发到终端，不落盘（对齐 compute-sanitizer）。
    EXPECT_NE(console.find("NPU_CHECK_APP_OUTPUT_MARKER"), std::string::npos);
    EXPECT_EQ(contents.str().find("NPU_CHECK_APP_OUTPUT_MARKER"), std::string::npos);
    // 工具自身的输出仍然照常落盘；infra 细节不打屏，只在文件与 plog 中。
    EXPECT_NE(contents.str().find("handshake=missing"), std::string::npos);
    EXPECT_EQ(console.find("handshake=missing"), std::string::npos);
    boost::filesystem::remove_all(directory);
}
TEST(ProcessRunnerTest, ScreenNoticeOnlyDescribesActionableFailures)
{
    ResultSummary summary;
    summary.outcome = Outcome::INFRA_FAILED;
    EXPECT_NE(ScreenNotice(summary).find("plog"), std::string::npos);

    summary.outcome = Outcome::APP_FAILED;
    summary.childExit = "3";
    EXPECT_NE(ScreenNotice(summary).find("non-zero status (exit=3)"), std::string::npos);
    summary.childExit = "signal:9";
    EXPECT_NE(ScreenNotice(summary).find("terminated by signal 9"), std::string::npos);

    summary.outcome = Outcome::FORWARDED;
    EXPECT_TRUE(ScreenNotice(summary).empty());
}
TEST(ProcessRunnerTest, InvalidLogPathDoesNotStartApplicationOrCreateDirectories)
{
    const auto directory =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("log-reject-%%%%-%%%%");
    boost::filesystem::create_directories(directory);
    const auto marker = directory / "started";
    Options options;
    options.application = {"/usr/bin/touch", marker.string()};
    options.workDir = directory.string();
    options.handshakeTimeoutMs = 100;
    for (const auto& path : {directory.string(), (directory / "missing/report.log").string()}) {
        options.logFile = path;
        EXPECT_EQ(RunApplication(options, "/missing/libnpu_check_processor.so"), 125);
        EXPECT_FALSE(boost::filesystem::exists(marker));
    }
    EXPECT_FALSE(boost::filesystem::exists(directory / "missing"));
    boost::filesystem::remove_all(directory);
}
} // namespace npucheck
