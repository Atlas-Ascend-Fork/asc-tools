/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "pm_sampling_artifacts.h"
#include <elf.h>
#include <sys/stat.h>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <utility>
// Use the already bundled Boost headers; keep JSON values typed (including empty
// arrays and numeric line/column fields) when normalizing LLVM's object stream.
#define BOOST_JSON_NO_LIB
#include <boost/json/src.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace npucompute {
namespace {
namespace fs = boost::filesystem;
namespace json = boost::json;
namespace process = boost::process;
constexpr int kBitsPerHexDigit = 4;
constexpr int kPcHexDigits = std::numeric_limits<std::uint64_t>::digits / kBitsPerHexDigit;
constexpr auto kToolPollInterval = std::chrono::milliseconds(10);
constexpr mode_t kPrivateFileMode = S_IRUSR | S_IWUSR;
constexpr std::string_view kHexPrefix = "0x";
constexpr unsigned kHexBase = 16;
constexpr unsigned kDecimalBase = 10;
void Require(bool ok, const std::string& message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}
// LLVM tools use hexadecimal addresses; SQLite stores the same numeric value as INTEGER.
std::string Hex(std::uint64_t pc)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(kPcHexDigits) << pc;
    return out.str();
}
struct TemporaryDirectory {
    fs::path path;
    ~TemporaryDirectory()
    {
        boost::system::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void Write(const fs::path& path, const char* data, std::size_t size)
{
    std::ofstream file(path.string(), std::ios::binary | std::ios::trunc);
    Require(bool(file), "cannot open output: " + path.string());
    file.write(data, static_cast<std::streamsize>(size));
    file.close();
    Require(bool(file), "cannot write output: " + path.string());
}
std::string Read(const fs::path& path)
{
    const auto size = fs::file_size(path);
    Require(
        size <= std::numeric_limits<std::size_t>::max() &&
            size <= static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()),
        "LLVM output size is not representable");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::ifstream file(path.string(), std::ios::binary);
    Require(bool(file), "cannot read LLVM output");
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Require(bool(file), "cannot read complete LLVM output");
    return bytes;
}
class Child {
public:
    process::group group;
    process::child handle;
    ~Child()
    {
        if (handle.valid() && handle.joinable()) {
            std::error_code ignored;
            group.terminate(ignored);
            handle.wait(ignored);
            group.detach();
        }
    }
};
void Run(
    const std::vector<std::string>& arguments, const fs::path& input, const fs::path& output, const fs::path& errors,
    const PmSamplingArtifactOptions& options)
{
    // Preserve private permissions and truncation when reusing the stderr file.
    for (const auto& path : {output, errors}) {
        const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kPrivateFileMode);
        Require(descriptor >= 0, "cannot open LLVM output: " + path.string() + ": " + std::strerror(errno));
        ::close(descriptor);
    }
    Child child;
    try {
        child.handle = process::child(
            arguments.front(), process::args(std::vector<std::string>(arguments.begin() + 1, arguments.end())),
            process::std_in<input.string(), process::std_out> output.string(), process::std_err > errors.string(),
            child.group);
    } catch (const std::exception& failure) {
        throw std::runtime_error("cannot start " + arguments.front() + ": " + failure.what());
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
    for (;;) {
        int exitStatus = 0;
        const auto waited = ::waitpid(child.handle.id(), &exitStatus, WNOHANG);
        if (waited == child.handle.id()) {
            child.handle.detach();
            child.group.detach();
            Require(
                WIFEXITED(exitStatus) && WEXITSTATUS(exitStatus) == 0,
                "LLVM tool failed: " + arguments[0] + ": " + Read(errors));
            return;
        }
        if (waited < 0 && errno == EINTR) {
            continue;
        }
        Require(waited >= 0, "cannot wait for LLVM tool");
        Require(std::chrono::steady_clock::now() < deadline, "LLVM tool timed out: " + arguments[0]);
        std::this_thread::sleep_for(kToolPollInterval);
    }
}
const std::vector<char>& Elf(const aclptiPmSamplingContext& context, PmSamplingElfSource source)
{
    Require(source == PmSamplingElfSource::Original || source == PmSamplingElfSource::Companion, "invalid ELF source");
    const auto& image = source == PmSamplingElfSource::Original ? context.originalElf : context.companionElf;
    Require(
        image && image->size() >= SELFMAG && std::memcmp(image->data(), ELFMAG, SELFMAG) == 0,
        "missing or invalid selected ELF");
    return *image;
}
std::uint64_t Address(const json::value& value)
{
    Require(value.is_string(), "LLVM Address is not a string");
    const std::string text(value.as_string().c_str());
    Require(
        text.size() > kHexPrefix.size() && text.compare(0, kHexPrefix.size(), kHexPrefix) == 0, "invalid LLVM Address");
    std::uint64_t pc = 0;
    for (std::size_t index = kHexPrefix.size(); index < text.size(); ++index) {
        const auto c = text[index];
        const unsigned digit =
            c >= '0' && c <= '9' ? static_cast<unsigned>(c) - static_cast<unsigned>('0') :
            c >= 'a' && c <= 'f' ? static_cast<unsigned>(c) - static_cast<unsigned>('a') + kDecimalBase :
            c >= 'A' && c <= 'F' ? static_cast<unsigned>(c) - static_cast<unsigned>('A') + kDecimalBase :
                                   kHexBase;
        Require(
            digit < kHexBase && pc <= (std::numeric_limits<std::uint64_t>::max() - digit) / kHexBase,
            "invalid LLVM Address");
        pc = pc * kHexBase + digit;
    }
    return pc;
}
json::array Normalize(const std::string& text)
{
    json::array records;
    std::size_t offset = 0;
    while (offset < text.size()) {
        while (offset < text.size() &&
               (text[offset] == ' ' || text[offset] == '\n' || text[offset] == '\r' || text[offset] == '\t')) {
            ++offset;
        }
        if (offset == text.size()) {
            break;
        }
        json::stream_parser parser;
        const auto consumed = parser.write_some(text.data() + offset, text.size() - offset);
        Require(consumed != 0, "invalid LLVM JSON stream");
        if (!parser.done()) {
            parser.finish();
        }
        auto value = parser.release();
        if (value.is_array()) {
            Require(records.empty(), "unexpected LLVM JSON array in object stream");
            records = std::move(value.as_array());
            offset += consumed;
            while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
                ++offset;
            }
            Require(offset == text.size(), "trailing LLVM JSON after array");
        } else {
            Require(value.is_object(), "LLVM JSON entry is not an object");
            records.push_back(std::move(value));
            offset += consumed;
        }
    }
    return records;
}
template <typename Generate>
bool AttemptArtifact(const fs::path& path, const Generate& generate, std::string& error)
{
    try {
        generate();
        return true;
    } catch (const std::exception& failure) {
        if (!error.empty()) {
            error += "\n";
        }
        error += path.filename().string() + ": LLVM artifact generation failed: " + failure.what();
        boost::system::error_code cleanupError;
        fs::remove(path, cleanupError);
        if (cleanupError) {
            error += "; partial output cleanup failed: " + cleanupError.message();
        } else {
            error += "; file was not retained";
        }
        return false;
    }
}

std::string ResolveToolDirectory(const PmSamplingArtifactOptions& options)
{
    auto tools = options.toolDirectory;
    if (tools.empty()) {
        const char* home = std::getenv("ASCEND_HOME_PATH");
        Require(home && *home, "ASCEND_HOME_PATH is not set");
        tools = (fs::path(home) / "tools/bisheng_compiler/bin").string();
    }
    Require(tools.find('\0') == std::string::npos, "invalid LLVM tool directory");
    return tools;
}

void GenerateDisassembly(
    const fs::path& temporary, const aclptiPmSamplingContext& context, const PmSamplingArtifactOptions& options,
    const fs::path& disassembly, PmSamplingArtifacts& artifacts)
{
    Require(options.timeoutMs > 0, "invalid LLVM execution timeout");
    const auto elf = temporary / "objdump.o";
    const auto& image = Elf(context, options.disassemblySource);
    Write(elf, image.data(), image.size());
    Run({(fs::path(ResolveToolDirectory(options)) / "llvm-objdump").string(), "-d", "-l", "-S", elf.string()},
        "/dev/null", disassembly, temporary / "objdump.stderr", options);
    Require(fs::file_size(disassembly) != 0, "empty LLVM disassembly");
    artifacts.disassemblyPath = disassembly.string();
}

struct SymbolizerResult {
    json::array records;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> addresses;
};
SymbolizerResult QuerySymbolizer(
    const fs::path& temporary, const aclptiPmSamplingContext& context, const PmSamplingResult& sampling,
    const PmSamplingArtifactOptions& options)
{
    const auto stackElf = temporary / "symbolizer.o";
    const auto& stackImage = Elf(context, options.pcStackSource);
    Write(stackElf, stackImage.data(), stackImage.size());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> addresses;
    std::string input;
    for (const auto& row : sampling.rows) {
        Require(
            context.businessInstructions->count(row.first) != 0, "non-business PC passed to LLVM artifact generator");
        const auto query = options.pcStackSource == PmSamplingElfSource::Original ?
                               row.first :
                               row.first - context.originalEntry + context.elfEntry + context.pcOffset;
        addresses.emplace_back(row.first, query);
        input += Hex(query) + "\n";
    }
    const auto stdinFile = temporary / "pcs.txt";
    Write(stdinFile, input.data(), input.size());
    const auto stderrFile = temporary / "tool.stderr";
    const auto rawStack = temporary / "symbolizer.json";
    Run({(fs::path(ResolveToolDirectory(options)) / "llvm-symbolizer").string(), "--obj=" + stackElf.string(),
         "--inlining=true", "--output-style=JSON"},
        stdinFile, rawStack, stderrFile, options);
    return {Normalize(Read(rawStack)), std::move(addresses)};
}

bool HasSourceLocation(const json::array& frames)
{
    bool resolved = false;
    for (const auto& frame : frames) {
        Require(frame.is_object(), "LLVM JSON frame is not an object");
        const auto* line = frame.as_object().if_contains("Line");
        const auto* file = frame.as_object().if_contains("FileName");
        resolved |= file && file->is_string() && !file->as_string().empty() && line &&
                    ((line->is_int64() && line->as_int64() > 0) || (line->is_uint64() && line->as_uint64() > 0));
    }
    return resolved;
}

std::uint64_t NormalizeSymbolizerRecords(SymbolizerResult& result, const PmSamplingArtifactOptions& options)
{
    auto& records = result.records;
    const auto& addresses = result.addresses;
    Require(records.size() == addresses.size(), "LLVM JSON address count mismatch");
    std::uint64_t unresolved = 0;
    for (std::size_t index = 0; index < records.size(); ++index) {
        Require(records[index].is_object(), "LLVM JSON entry is not an object");
        auto& object = records[index].as_object();
        Require(
            object.contains("Address") && Address(object.at("Address")) == addresses[index].second &&
                object.contains("Symbol") && object.at("Symbol").is_array(),
            "LLVM JSON record does not match requested PC");
        const bool resolved = HasSourceLocation(object.at("Symbol").as_array());
        if (!resolved) {
            ++unresolved;
        }
        if (options.pcStackSource == PmSamplingElfSource::Companion) {
            object["SymbolizerAddress"] = Hex(addresses[index].second);
        }
        object["Address"] = Hex(addresses[index].first); // Same numeric value as SQLite Address.
        // ModuleName describes the selected ELF, without exposing a deleted temporary path.
        object["ModuleName"] =
            options.pcStackSource == PmSamplingElfSource::Original ? "PmSamplingOriginal.o" : "PmSamplingCompanion.o";
    }
    return unresolved;
}

void WriteEmptyPcStack(const fs::path& path, PmSamplingArtifacts& artifacts)
{
    constexpr std::string_view emptyStack = "[]";
    Write(path, emptyStack.data(), emptyStack.size());
    artifacts.pcStackPath = path.string();
}

void GeneratePcStack(
    const fs::path& temporary, const aclptiPmSamplingContext* context, const PmSamplingResult& sampling,
    const PmSamplingArtifactOptions& options, const fs::path& stackPath, PmSamplingArtifacts& artifacts)
{
    if (sampling.rows.empty()) {
        WriteEmptyPcStack(stackPath, artifacts);
        return;
    }
    Require(context != nullptr, "missing PmSampling ELF context");
    Require(context->businessInstructions != nullptr, "missing PmSampling business instruction map for symbolization");
    Require(options.timeoutMs > 0, "invalid LLVM execution timeout");
    auto result = QuerySymbolizer(temporary, *context, sampling, options);
    const auto unresolved = NormalizeSymbolizerRecords(result, options);
    const auto serialized = json::serialize(result.records);
    Write(stackPath, serialized.data(), serialized.size());
    artifacts.pcStackPath = stackPath.string();
    artifacts.unresolvedPcCount = unresolved;
}

} // namespace

bool GeneratePmSamplingArtifacts(
    const std::string& stagingDirectory, const aclptiPmSamplingContext& context, const PmSamplingResult& sampling,
    const PmSamplingArtifactOptions& options, PmSamplingArtifacts& artifacts, std::string& error)
{
    return GeneratePmSamplingArtifacts(stagingDirectory, &context, sampling, options, artifacts, error);
}

bool GeneratePmSamplingArtifacts(
    const std::string& stagingDirectory, const aclptiPmSamplingContext* context, const PmSamplingResult& sampling,
    const PmSamplingArtifactOptions& options, PmSamplingArtifacts& artifacts, std::string& error)
{
    artifacts = {};
    error.clear();
    try {
        Require(
            !stagingDirectory.empty() && stagingDirectory.find('\0') == std::string::npos &&
                fs::is_directory(stagingDirectory),
            "missing PmSampling staging directory");
        const auto disassembly = fs::path(stagingDirectory) / "objdump.txt";
        const auto stackPath = fs::path(stagingDirectory) / "symbolizer.json";
        if (!context && sampling.rows.empty()) {
            return AttemptArtifact(stackPath, [&] { WriteEmptyPcStack(stackPath, artifacts); }, error);
        }
        std::string pattern = (fs::path(stagingDirectory) / ".PmSampling-XXXXXX").string();
        Require(::mkdtemp(pattern.data()) != nullptr, "cannot create LLVM temporary directory");
        TemporaryDirectory temporary{pattern};
        const bool disassemblyOk = AttemptArtifact(
            disassembly,
            [&] {
                Require(context != nullptr, "missing PmSampling ELF context");
                GenerateDisassembly(temporary.path, *context, options, disassembly, artifacts);
            },
            error);
        const bool stackOk = AttemptArtifact(
            stackPath, [&] { GeneratePcStack(temporary.path, context, sampling, options, stackPath, artifacts); },
            error);
        return disassemblyOk && stackOk;
    } catch (const std::exception& failure) {
        error = "PmSampling LLVM artifact setup failed: " + std::string(failure.what());
        return false;
    }
}
} // namespace npucompute
