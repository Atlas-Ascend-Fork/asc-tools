/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "process_runner.h"

#include "uds_client.h"
#include "npu_tool_log.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <string_view>
#include <thread>
#include <utility>
#include <unistd.h>

namespace npucheck {
namespace {

constexpr std::string_view SIGNAL_EXIT_PREFIX = "signal:";

volatile sig_atomic_t g_childProcessGroup = -1;

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() { Reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    int Get() const noexcept { return fd_; }

    int Release() noexcept
    {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void Reset(int fd = -1) noexcept
    {
        if (fd_ >= 0) {
            (void)close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

void ForwardSignal(int signalNumber)
{
    const int savedErrno = errno;
    const sig_atomic_t processGroup = g_childProcessGroup;
    if (processGroup > 0) {
        (void)kill(-static_cast<pid_t>(processGroup), signalNumber);
    }
    errno = savedErrno;
}

class SignalForwarder {
public:
    SignalForwarder(pid_t childProcessGroup, std::string& error)
    {
        struct sigaction action {};
        action.sa_handler = ForwardSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        g_childProcessGroup = static_cast<sig_atomic_t>(childProcessGroup);
        for (size_t index = 0; index < signals_.size(); ++index) {
            if (sigaction(signals_[index], &action, &previous_[index]) != 0) {
                error = std::string("sigaction: ") + std::strerror(errno);
                Restore();
                return;
            }
            ++installed_;
        }
        good_ = true;
    }

    ~SignalForwarder() { Restore(); }

    SignalForwarder(const SignalForwarder&) = delete;
    SignalForwarder& operator=(const SignalForwarder&) = delete;

    bool Good() const { return good_; }

private:
    void Restore()
    {
        while (installed_ != 0) {
            --installed_;
            (void)sigaction(signals_[installed_], &previous_[installed_], nullptr);
        }
        g_childProcessGroup = -1;
        good_ = false;
    }

    inline static constexpr std::array signals_{SIGINT, SIGTERM, SIGHUP};
    std::array<struct sigaction, signals_.size()> previous_{};
    size_t installed_ = 0;
    bool good_ = false;
};

class SignalBlocker {
public:
    explicit SignalBlocker(std::string& error)
    {
        sigemptyset(&blocked_);
        sigaddset(&blocked_, SIGINT);
        sigaddset(&blocked_, SIGTERM);
        sigaddset(&blocked_, SIGHUP);
        if (sigprocmask(SIG_BLOCK, &blocked_, &previous_) != 0) {
            error = std::string("sigprocmask(SIG_BLOCK): ") + std::strerror(errno);
            return;
        }
        active_ = true;
    }

    ~SignalBlocker() { (void)RestoreSignalMask(); }

    SignalBlocker(const SignalBlocker&) = delete;
    SignalBlocker& operator=(const SignalBlocker&) = delete;

    bool Good() const { return active_; }

    bool Restore() { return RestoreSignalMask() == 0; }

    bool Restore(std::string& error)
    {
        const int result = RestoreSignalMask();
        if (result != 0) {
            error = std::string("sigprocmask(SIG_SETMASK): ") + std::strerror(result);
        }
        return result == 0;
    }

private:
    int RestoreSignalMask() noexcept
    {
        if (!active_) {
            return 0;
        }
        if (sigprocmask(SIG_SETMASK, &previous_, nullptr) != 0) {
            return errno;
        }
        active_ = false;
        return 0;
    }

    sigset_t blocked_{};
    sigset_t previous_{};
    bool active_ = false;
};

// infra_failed 具体原因的统一落盘出口：不打屏。这类细节（握手缺失、报告不完整等）
// 只能说明流程断在哪一环，说不了根因是谁 —— 打在终端上容易被当成结论（例如目标程序
// 根本不是 ACL 应用、或应用自身中途被杀，都会表现为"工具侧失败"）。告知由 ScreenNotice
// 的通用提示承担，细节进 plog 与 --log-file 供排查。
void LogInfraDetail(const std::string& message) { ASCTOOL_ERROR("[INFRA] %s", message.c_str()); }

// 报告出口。四条流各有去向，但都必须串行化 —— 否则应用输出与工具消息会在字节级交错。
//
// --log-file 只收工具自身的输出（报告与工具消息），不转录应用输出：应用是子进程，
// 其打印直接写继承来的终端流
class OutputSink {
public:
    explicit OutputSink(const std::string& logPath) : logRequired_(!logPath.empty())
    {
        if (!logPath.empty()) {
            // 覆盖而非追加：同一路径重复跑时，上一次的报告必须被清掉，否则用户读到的是
            // 多次运行拼在一起的内容，很容易把上一轮的诊断当成本轮结论。
            log_.open(logPath, std::ios::binary | std::ios::trunc);
        }
    }

    bool Good() const { return !logRequired_ || log_.is_open(); }

    // 应用自身的 stdout/stderr：原样转发到终端，不加任何前缀，不改写。打屏行为不受
    // --log-file 影响，该文件也只收工具自身的输出。
    void Console(const char* data, size_t size)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WriteFd(STDOUT_FILENO, data, size);
    }

    // 权威报告的输出通道：原样写出，不加 "npu_check: " 前缀，也不拆行。报告是给人读的
    // 多行文本，逐行加前缀只会破坏渲染器精心排好的版式。
    void Report(const std::string& text)
    {
        if (text.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // 指定了 --log-file 时报告只入文件，不再打屏（2.3.5 报告出口表）。两处都写会让
        // 用户在终端和文件里各读到一份，重定向采集时还会重复计数。应用自身的
        // stdout/stderr 不受影响，仍照常转发。
        if (!log_.is_open()) {
            WriteFd(STDOUT_FILENO, text.data(), text.size());
            if (text.back() != '\n') {
                const char newline = '\n';
                WriteFd(STDOUT_FILENO, &newline, 1);
            }
        }
        if (log_.is_open()) {
            log_.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (text.back() != '\n') {
                log_.put('\n');
            }
            log_.flush();
        }
    }

    void Sanitizer(const std::string& message, bool error = false)
    {
        std::string line = "npu_check: " + message;
        if (line.back() != '\n') {
            line.push_back('\n');
        }
        std::lock_guard<std::mutex> lock(mutex_);
        WriteFd(error ? STDERR_FILENO : STDOUT_FILENO, line.data(), line.size());
        if (log_.is_open()) {
            log_.write(line.data(), static_cast<std::streamsize>(line.size()));
            log_.flush();
        }
    }

    // infra_failed 的具体原因：不进终端，只落 plog 与 --log-file（见 LogInfraDetail）。
    void Detail(const std::string& message)
    {
        LogInfraDetail(message);
        std::string line = "npu_check: " + message;
        if (line.back() != '\n') {
            line.push_back('\n');
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (log_.is_open()) {
            log_.write(line.data(), static_cast<std::streamsize>(line.size()));
            log_.flush();
        }
    }

private:
    static void WriteFd(int fd, const char* data, size_t size)
    {
        size_t offset = 0;
        while (offset < size) {
            const ssize_t result = write(fd, data + offset, size - offset);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return;
            }
            offset += static_cast<size_t>(result);
        }
    }

    mutable std::mutex mutex_;
    std::ofstream log_;
    bool logRequired_ = false;
};

bool ReadRandomDevice(unsigned char* output, size_t size)
{
    int fd = -1;
    do {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return false;
    }

    UniqueFd randomDevice(fd);
    size_t total = 0;
    while (total < size) {
        const ssize_t result = read(randomDevice.Get(), output + total, size - total);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        total += static_cast<size_t>(result);
    }
    return true;
}

bool RandomBytes(void* output, size_t size)
{
    auto* cursor = static_cast<unsigned char*>(output);
    size_t total = 0;
#ifdef SYS_getrandom
    while (total < size) {
        const ssize_t result = syscall(SYS_getrandom, cursor + total, size - total, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && errno == ENOSYS) {
            break;
        }
        if (result <= 0) {
            return false;
        }
        total += static_cast<size_t>(result);
    }
    if (total == size) {
        return true;
    }
#endif
    return ReadRandomDevice(cursor + total, size - total);
}

// 生成 32 个十六进制字符（16 字节随机数）。失败返回空串。
// 唯一用途是 UDS 抽象地址的随机段；会话 nonce 已随协议简化删除。
std::string HexRandom()
{
    constexpr size_t UDS_RANDOM_BYTES = 16;
    constexpr size_t HEX_DIGITS_PER_BYTE = 2;
    std::array<unsigned char, UDS_RANDOM_BYTES> bytes{};
    if (!RandomBytes(bytes.data(), bytes.size())) {
        return {};
    }
    constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::string text(bytes.size() * HEX_DIGITS_PER_BYTE, '0');
    for (size_t index = 0; index < bytes.size(); ++index) {
        text[index * HEX_DIGITS_PER_BYTE] = HEX_DIGITS[bytes[index] >> 4u];
        text[index * HEX_DIGITS_PER_BYTE + 1] = HEX_DIGITS[bytes[index] & 0x0fu];
    }
    return text;
}

bool CreateSessionDirectory(std::string& directory, std::string& error)
{
    std::string pattern = "/tmp/npu_check-XXXXXX";
    if (mkdtemp(pattern.data()) == nullptr) {
        error = std::string("mkdtemp: ") + std::strerror(errno);
        return false;
    }
    if (chmod(pattern.c_str(), S_IRWXU) != 0) {
        const int chmodError = errno;
        (void)rmdir(pattern.c_str());
        error = std::string("chmod session directory: ") + std::strerror(chmodError);
        return false;
    }
    directory = std::move(pattern);
    return true;
}

bool EnsureDirectory(const std::string& path, std::string& error)
{
    boost::system::error_code filesystemError;
    boost::filesystem::create_directories(path, filesystemError);
    if (filesystemError) {
        error = "cannot create directory '" + path + "': " + filesystemError.message();
        return false;
    }
    return true;
}

std::vector<char*> BuildArgv(std::vector<std::string>& arguments)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    return argv;
}

int ChildExitCode(int status)
{
    constexpr int SIGNAL_EXIT_CODE_BASE = 128;
    if (WIFSIGNALED(status)) {
        return SIGNAL_EXIT_CODE_BASE + WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : INFRASTRUCTURE_ERROR_EXIT_CODE;
}

// 摘要行里的 child_exit 字段。被信号杀死与正常退出必须能区分开：两者映射到同一个
// 数值空间时，"退出码 137" 和 "被 SIGKILL 杀死" 就再也分不出来了。
std::string FormatChildExit(int status)
{
    if (WIFSIGNALED(status)) {
        return std::string(SIGNAL_EXIT_PREFIX) + std::to_string(WTERMSIG(status));
    }
    return WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : "unknown";
}

// 把 Options 里的工具集合转成线路上的 Configure 请求。解析阶段已经保证了排序与去重，
// 这里只做搬运，不再重新规范化 —— 若两处各做一遍，规则一旦分叉就很难发现。
npucheck::ipc::ConfigureRequest BuildConfigureRequest(const Options& options)
{
    npucheck::ipc::ConfigureRequest request;
    request.globalFlags = 0;
    request.tools = options.tools;
    return request;
}

} // namespace

std::string FormatResultSummary(const ResultSummary& summary)
{
    const char* outcome = "infra_failed";
    switch (summary.outcome) {
        case Outcome::FORWARDED:
            outcome = "forwarded";
            break;
        case Outcome::APP_FAILED:
            outcome = "app_failed";
            break;
        case Outcome::INFRA_FAILED:
            outcome = "infra_failed";
            break;
    }
    std::string hasErrors = "unknown";
    if (summary.hasErrors == 0) {
        hasErrors = "0";
    } else if (summary.hasErrors > 0) {
        hasErrors = "1";
    }
    return std::string("[CLI] outcome=") + outcome + " has_errors=" + hasErrors +
           " truncated=" + (summary.truncated ? "1" : "0") + " child_exit=" + summary.childExit +
           " exit=" + std::to_string(summary.exit);
}

void LogResultSummary(const ResultSummary& summary)
{
    // 摘要行是给工具维护者看的机器格式，常态化落 plog；终端上只允许出现 ScreenNotice
    // 给出的人类可读提示，两套输出不得混用。
    const std::string line = FormatResultSummary(summary);
    switch (summary.outcome) {
        case Outcome::FORWARDED:
            ASCTOOL_INFO("%s", line.c_str());
            break;
        case Outcome::APP_FAILED:
            ASCTOOL_WARNING("%s", line.c_str());
            break;
        case Outcome::INFRA_FAILED:
            ASCTOOL_ERROR("%s", line.c_str());
            break;
    }
}

// 只有能给出明确说法的结果才允许打屏：infra_failed 告知工具侧失败并指向 plog，
// app_failed 告知应用退出状态。forwarded 一切正常，has_errors 的结论已随报告本身
// 输出，都不需要额外提示。
std::string ScreenNotice(const ResultSummary& summary)
{
    switch (summary.outcome) {
        case Outcome::INFRA_FAILED:
            return "npu_check: tool execution failed; check the plog for details";
        case Outcome::APP_FAILED:
            if (summary.childExit.compare(0, SIGNAL_EXIT_PREFIX.size(), SIGNAL_EXIT_PREFIX) == 0) {
                return "npu_check: application terminated by signal " +
                       summary.childExit.substr(SIGNAL_EXIT_PREFIX.size());
            }
            if (summary.childExit != "none" && summary.childExit != "unknown") {
                return "npu_check: application exited with non-zero status (exit=" + summary.childExit + ")";
            }
            return "npu_check: application exited with non-zero status";
        case Outcome::FORWARDED:
            break;
    }
    return {};
}

int RunApplication(const Options& options, const std::string& libraryPath)
{
    // 摘要必须在任何返回路径上都落一次 plog、终端提示也只在这一处统一给出，因此用
    // RAII 守卫兜住，而不是在每个 return 前手写一遍 —— 后者只要漏掉一处，那条路径
    // 就既没有落盘记录、用户也得不到任何提示。
    ResultSummary summary;
    struct SummaryGuard {
        const ResultSummary& summary;
        ~SummaryGuard()
        {
            LogResultSummary(summary);
            const std::string notice = ScreenNotice(summary);
            if (!notice.empty()) {
                std::cerr << notice << '\n';
            }
        }
    } summaryGuard{summary};

    std::string sessionDirectory;
    std::string error;
    if (!CreateSessionDirectory(sessionDirectory, error)) {
        LogInfraDetail(error);
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }

    // 会话标识：它在每一帧都被校验，覆盖面严格大于原先只在 Hello 校验一次的 nonce，
    // 因此 nonce 已删除（见 wire_protocol.h 的说明）。
    uint64_t sessionId = 0;
    if (!RandomBytes(&sessionId, sizeof(sessionId)) || sessionId == 0) {
        LogInfraDetail("cannot generate session identity");
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }

    // --work-dir 只控制 CLI 会话日志。DBI 使用自己管理的私有 runtime/cache 目录。
    const std::string workDir = options.workDir.empty() ? sessionDirectory : options.workDir;
    if (!EnsureDirectory(workDir, error)) {
        LogInfraDetail(error);
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }

    const std::string& logPath = options.logFile;
    if (!logPath.empty() && !ValidateLogFilePath(logPath, error)) {
        LogInfraDetail(error);
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }

    // UDS 地址走抽象命名空间，不在文件系统里创建 socket 文件。前导 '@' 是与注入库之间
    // 的书写约定，两端填 sockaddr_un 时会把它换成 '\0'（见 BuildAbstractAddress）。
    // 随机部分保证并发实例之间不撞车；它不是秘密 —— /proc/net/unix 任何 UID 都能读到，
    // 身份校验完全由 SO_PEERCRED 承担。
    const std::string udsSuffix = HexRandom();
    if (udsSuffix.empty()) {
        LogInfraDetail("cannot generate a UDS address");
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }
    const std::string udsName = "@npu_check-" + udsSuffix;
    OutputSink output(logPath);
    if (!output.Good()) {
        LogInfraDetail("cannot open '" + logPath + "' for writing");
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }

    const auto configure = BuildConfigureRequest(options);
    std::string toolNames;
    for (const auto& tool : options.tools) {
        toolNames += (toolNames.empty() ? "" : ",");
        toolNames += npucheck::ipc::ToolName(tool.toolId);
    }
    ASCTOOL_DEBUG("[INJECTION] library=%s result=resolved", libraryPath.c_str());

    int consolePipe[2] = {-1, -1};
    if (pipe2(consolePipe, O_CLOEXEC) != 0) {
        output.Detail(std::string("pipe2: ") + std::strerror(errno));
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }
    UniqueFd consoleRead(consolePipe[0]);
    UniqueFd consoleWrite(consolePipe[1]);
    SignalBlocker signalBlocker(error);
    if (!signalBlocker.Good()) {
        output.Detail(error);
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }

    // T0 必须取在 fork 之前的最后一步。
    //
    // 用户感知的是"从敲回车到应用真正开跑"这一整段，其中最不可控的恰恰是 exec 与
    // CANN 动态库加载。若把起点挪到 fork 之后、甚至 connect 成功之后，这段耗时就不
    // 占预算，用户设定的超时形同虚设。
    const npucheck::ipc::DeadlineMs handshakeDeadline = npucheck::ipc::DeadlineAfterMs(options.handshakeTimeoutMs);

    const pid_t child = fork();
    if (child < 0) {
        output.Detail(std::string("fork: ") + std::strerror(errno));
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }
    if (child == 0) {
        constexpr int CHILD_SETUP_ERROR_EXIT_CODE = 127;
        if (setpgid(0, 0) != 0) {
            dprintf(STDERR_FILENO, "npu_check: setpgid failed: %s\n", std::strerror(errno));
            _exit(CHILD_SETUP_ERROR_EXIT_CODE);
        }
        if (!signalBlocker.Restore()) {
            dprintf(STDERR_FILENO, "npu_check: cannot restore child signal mask\n");
            _exit(CHILD_SETUP_ERROR_EXIT_CODE);
        }
        consoleRead.Reset();
        if (dup2(consoleWrite.Get(), STDOUT_FILENO) < 0 || dup2(consoleWrite.Get(), STDERR_FILENO) < 0) {
            _exit(CHILD_SETUP_ERROR_EXIT_CODE);
        }
        consoleWrite.Reset();
        const std::string sessionText = std::to_string(sessionId);
        const std::string parentText = std::to_string(getppid());
        const std::string timeoutText = std::to_string(options.handshakeTimeoutMs);
        (void)setenv("ACL_API_INJECTION", libraryPath.c_str(), 1);
        (void)setenv(npucheck::ipc::UDS_NAME_ENV, udsName.c_str(), 1);
        (void)setenv(npucheck::ipc::SESSION_ID_ENV, sessionText.c_str(), 1);
        (void)setenv(npucheck::ipc::CLI_PID_ENV, parentText.c_str(), 1);
        (void)setenv(npucheck::ipc::HANDSHAKE_TIMEOUT_ENV, timeoutText.c_str(), 1);
        // 工作目录经环境变量下发：Configure 改用注册表编码后只承载工具与子选项，
        // 路径这类与协议无关的部署信息不再占线路。
        (void)setenv(npucheck::ipc::WORK_DIR_ENV, workDir.c_str(), 1);
        auto arguments = options.application;
        auto argv = BuildArgv(arguments);
        execvp(argv[0], argv.data());
        dprintf(STDERR_FILENO, "npu_check: execvp failed: %s\n", std::strerror(errno));
        _exit(CHILD_SETUP_ERROR_EXIT_CODE);
    }
    consoleWrite.Reset();
    if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        output.Detail(std::string("setpgid: ") + std::strerror(errno));
        (void)kill(child, SIGKILL);
        (void)waitpid(child, nullptr, 0);
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }
    SignalForwarder signalForwarder(child, error);
    if (!signalForwarder.Good()) {
        output.Detail(error);
        if (kill(-child, SIGKILL) != 0) {
            (void)kill(child, SIGKILL);
        }
        (void)waitpid(child, nullptr, 0);
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }
    if (!signalBlocker.Restore(error)) {
        output.Detail(error);
        if (kill(-child, SIGKILL) != 0) {
            (void)kill(child, SIGKILL);
        }
        (void)waitpid(child, nullptr, 0);
        return summary.exit = INFRASTRUCTURE_ERROR_EXIT_CODE;
    }

    ASCTOOL_DEBUG(
        "[CLI] session=%llu tools=%s app_pid=%d app_pgid=%d", static_cast<unsigned long long>(sessionId),
        toolNames.c_str(), static_cast<int>(child), static_cast<int>(child));
    std::atomic<bool> childExited{false};
    std::thread consoleReader([&output, &childExited, fd = std::move(consoleRead)] {
        constexpr size_t CONSOLE_BUFFER_SIZE = 8192;
        constexpr int CONSOLE_POLL_INTERVAL_MS = 100;
        std::array<char, CONSOLE_BUFFER_SIZE> buffer{};
        std::chrono::steady_clock::time_point drainDeadline{};
        while (true) {
            pollfd descriptor{};
            descriptor.fd = fd.Get();
            descriptor.events = POLLIN;
            int pollResult = 0;
            do {
                pollResult = poll(&descriptor, 1, CONSOLE_POLL_INTERVAL_MS);
            } while (pollResult < 0 && errno == EINTR);
            if (pollResult < 0 || (static_cast<unsigned int>(descriptor.revents) & (POLLERR | POLLNVAL)) != 0) {
                break;
            }
            if ((static_cast<unsigned int>(descriptor.revents) & POLLIN) != 0) {
                const ssize_t bytes = read(fd.Get(), buffer.data(), buffer.size());
                if (bytes < 0 && errno == EINTR) {
                    continue;
                }
                if (bytes <= 0) {
                    break;
                }
                output.Console(buffer.data(), static_cast<size_t>(bytes));
            } else if ((static_cast<unsigned int>(descriptor.revents) & POLLHUP) != 0) {
                break;
            }
            if (!childExited) {
                continue;
            }
            if (drainDeadline.time_since_epoch().count() == 0) {
                drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            } else if (std::chrono::steady_clock::now() >= drainDeadline) {
                break;
            }
        }
    });

    ipc::UdsClient client;
    const bool handshake = client.ConnectAndConfigure(
        udsName, sessionId, static_cast<uint32_t>(child), handshakeDeadline, configure, error);
    // Result 分片的拼接缓冲。只有 receiver 线程写，join 之后主线程才读。
    std::string result;
    std::atomic<bool> resultComplete{false};
    std::atomic<bool> resultHasErrors{false};
    std::atomic<bool> resultTruncated{false};
    std::atomic<uint64_t> resultFrames{0};
    std::atomic<bool> protocolComplete{handshake};
    std::thread receiver;
    if (handshake) {
        receiver = std::thread([&] {
            while (true) {
                npucheck::ipc::Frame frame{};
                std::string receiveError;
                // 采集阶段不设超时：应用可能跑数小时，任何固定值都是错的。终止由
                // 对端关闭连接（CLOSED）或子进程退出驱动，见下面的分支。
                const npucheck::ipc::IoStatus status = client.Receive(frame, npucheck::ipc::NO_DEADLINE, receiveError);
                if (status == npucheck::ipc::IoStatus::CLOSED) {
                    break;
                }
                if (status == npucheck::ipc::IoStatus::TIMEOUT) {
                    if (childExited) {
                        break;
                    }
                    continue;
                }
                if (status != npucheck::ipc::IoStatus::OK) {
                    protocolComplete = false;
                    output.Detail("UDS receive failed: " + receiveError);
                    break;
                }
                if (frame.type == npucheck::ipc::MessageType::RESULT) {
                    std::string message;
                    if (!npucheck::ipc::DecodeText(frame.payload, message, receiveError)) {
                        protocolComplete = false;
                        output.Detail("UDS payload failed: " + receiveError);
                        break;
                    }
                    // 上限与注入库侧共用同一个常量：对端异常时不能让 CLI 一直吃内存。
                    if (result.size() + message.size() > npucheck::ipc::MAX_RESULT_BYTES) {
                        protocolComplete = false;
                        output.Detail("result exceeds the maximum report size");
                        break;
                    }
                    result.append(message);
                    ++resultFrames;
                    if ((frame.flags & npucheck::ipc::FLAG_MORE) != 0) {
                        continue;
                    }
                    // MORE=0 的末帧到达即报告完整；结论位只在这一帧上有效，前面的分片
                    // 不带任何结论，所以这里必须读末帧的 flags 而不是任意一帧的。
                    resultHasErrors = (frame.flags & npucheck::ipc::FLAG_HAS_ERRORS) != 0;
                    resultTruncated = (frame.flags & npucheck::ipc::FLAG_TRUNCATED) != 0;
                    resultComplete = true;
                    break;
                }
                if (frame.type == npucheck::ipc::MessageType::ERROR) {
                    // Error 表示注入库侧基础设施失败，本次检查没有可信结论，不再等 Result。
                    protocolComplete = false;
                    npucheck::ipc::ErrorPayload failure{};
                    std::string decodeError;
                    if (!npucheck::ipc::DecodeError(frame.payload, failure, decodeError)) {
                        output.Detail("ERROR malformed error payload: " + decodeError);
                        break;
                    }
                    // domain/code 是稳定取值进结构化日志，message 只原样转述给人看，
                    // 不参与任何判定。
                    ASCTOOL_DEBUG(
                        "[UDS] phase=error domain=%u code=%u", static_cast<unsigned>(failure.domain),
                        static_cast<unsigned>(failure.code));
                    output.Detail("ERROR " + failure.message);
                    break;
                }
                // 其余都是 must-ignore 的实时诊断，收到即打印，不进拼接缓冲。
                std::string message;
                if (!npucheck::ipc::DecodeText(frame.payload, message, receiveError)) {
                    protocolComplete = false;
                    output.Sanitizer("UDS payload failed: " + receiveError, true);
                    break;
                }
                output.Sanitizer(std::string(npucheck::ipc::MessageTypeName(frame.type)) + " " + message, true);
            }
        });
    } else {
        protocolComplete = false;
        ASCTOOL_DEBUG("[UDS] phase=handshake result=failed");
        output.Detail("handshake=missing reason=\"" + error + "\"");
    }

    int childStatus = 0;
    bool childStatusAvailable = true;
    while (waitpid(child, &childStatus, 0) < 0) {
        if (errno != EINTR) {
            output.Detail(std::string("waitpid: ") + std::strerror(errno));
            childStatusAvailable = false;
            break;
        }
    }
    childExited = true;
    consoleReader.join();
    if (receiver.joinable()) {
        receiver.join();
    }
    const int childExit = childStatusAvailable ? ChildExitCode(childStatus) : INFRASTRUCTURE_ERROR_EXIT_CODE;

    // 判定顺序是固定的：先看有没有收到 MORE=0 的末帧，再看是不是被 Error 打断，
    // 最后才是"连接断了但报告没收全"。
    if (resultComplete) {
        ASCTOOL_DEBUG(
            "[UDS] phase=result frames=%llu bytes=%zu truncated=%d has_errors=%d",
            static_cast<unsigned long long>(resultFrames.load()), result.size(), static_cast<int>(resultTruncated),
            static_cast<int>(resultHasErrors));
        output.Report(result);
        if (resultTruncated) {
            output.Sanitizer("report truncated: the diagnostic buffer reached its size limit", true);
        }
    } else if (handshake) {
        // 报告缺失或截断：已经收到的分片一律丢弃。半份报告看上去和完整报告没有区别，
        // 输出它等于让用户把"没查到问题"和"没查完"混为一谈。
        ASCTOOL_DEBUG(
            "[UDS] phase=result frames=%llu bytes=%zu truncated=unknown has_errors=unknown",
            static_cast<unsigned long long>(resultFrames.load()), result.size());
        output.Detail("result missing or truncated; the partial report was discarded");
    }

    summary.truncated = resultTruncated;
    summary.childExit = childStatusAvailable ? FormatChildExit(childStatus) : "unknown";
    int exitCode = 0;
    if (!handshake || !protocolComplete || !resultComplete || !childStatusAvailable) {
        // 基础设施失败：拿不到可信结论，退 125 与应用自身的退出码区分开。
        // has_errors 保持 unknown —— 没收到完整 Result 就没有结论可言。
        summary.outcome = Outcome::INFRA_FAILED;
        exitCode = INFRASTRUCTURE_ERROR_EXIT_CODE;
    } else {
        summary.hasErrors = resultHasErrors ? 1 : 0;
        // 默认透传应用退出码：检出问题不等于运行失败，是否要让 CI 挂掉由调用方决定。
        // 只有显式指定 --error-exitcode 时才用它覆盖。
        exitCode = childExit;
        if (resultHasErrors && options.errorExitCode != 0) {
            exitCode = options.errorExitCode;
        }
        // 检出问题不算 app_failed：检查跑完了、报告也拿到了，结论由 has_errors 承载。
        summary.outcome = childExit == 0 ? Outcome::FORWARDED : Outcome::APP_FAILED;
    }
    summary.exit = exitCode;

    // 只删自己 mkdtemp 出来的会话目录。--work-dir 指定的目录是用户的，即便本次的
    // probe 缓存就落在里面，也一律不碰 —— 递归删一个用户给的路径
    // 是不可逆的，代价远大于留下几个文件。
    boost::system::error_code cleanupError;
    boost::filesystem::remove_all(sessionDirectory, cleanupError);
    return exitCode;
}

} // namespace npucheck
