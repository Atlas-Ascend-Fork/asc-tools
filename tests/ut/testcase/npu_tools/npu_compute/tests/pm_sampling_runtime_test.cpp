/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "runtime/npu_compute_runtime.h"
#include "artifact_sink.h"
#include "pm_sampling/pm_sampling_types.h"
#include "npu_compute/acl_pti_callback_stub.h"
#include "sqlite_test_api.h"
#include "cli/report/summary_sqlite_finalizer.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace sqlite = npucompute::sqlite;

namespace fs = std::filesystem;
namespace {
aclptiProfilingDataCallback callback;
#define CHECK(x)                                                                                 \
    do {                                                                                         \
        if (!(x))                                                                                \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x); \
    } while (false)
struct Directory {
    fs::path path;
    ~Directory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
std::set<fs::path> StagingDirectories(const fs::path& parent)
{
    std::set<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(parent)) {
        if (entry.path().filename().string().find(".npu-compute-pm-sampling-") == 0)
            paths.insert(entry.path());
    }
    return paths;
}
std::string Read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void CheckDatabase(const fs::path& path, bool zero)
{
    sqlite3* db = nullptr;
    CHECK(sqlite::GetApi().openV2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    // A receiver must see a complete standalone database, with no open writer transaction.
    CHECK(sqlite::GetApi().exec(db, "BEGIN EXCLUSIVE") == SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    CHECK(
        sqlite::GetApi().prepareV2(
            db, "SELECT address, ibuf_empty, nop_cycles, scoreboard_not_ready FROM PcSampling", -1, &query, nullptr) ==
        SQLITE_OK);
    CHECK(sqlite::GetApi().step(query) == SQLITE_ROW);
    CHECK(sqlite::GetApi().columnType(query, 0) == SQLITE_INTEGER && sqlite::GetApi().columnInt64(query, 0) == 0x20);
    CHECK(sqlite::GetApi().columnInt64(query, 1) == (zero ? 0 : 45));
    CHECK(sqlite::GetApi().columnInt64(query, 2) == (zero ? 0 : 47));
    CHECK(sqlite::GetApi().columnInt64(query, 3) == (zero ? 0 : 127));
    CHECK(sqlite::GetApi().step(query) == SQLITE_DONE);
    sqlite::GetApi().finalize(query);
    CHECK(sqlite::GetApi().exec(db, "ROLLBACK") == SQLITE_OK);
    CHECK(sqlite::GetApi().close(db) == SQLITE_OK);
    CHECK(!fs::exists(path.string() + "-wal") && !fs::exists(path.string() + "-journal"));
}
class Sink final : public npucompute::ArtifactSink {
public:
    fs::path output;
    std::string failure;
    std::ofstream current;
    std::string name;
    std::vector<std::string> committed;
    std::size_t samplingBegins = 0;
    std::size_t aborts = 0;
    void Begin(const std::string& artifact) override
    {
        name = artifact;
        if (fs::path(name).filename() == "summary.db") {
            if (samplingBegins++ == 0) {
                bool complete = false;
                for (const auto& path : StagingDirectories("/tmp")) {
                    complete |= fs::exists(path / "summary.db");
                }
                CHECK(complete);
            }
        }
        if (failure == "sink-begin" && name.find("summary.db") != std::string::npos) {
            throw std::runtime_error("injected begin failure");
        }
        fs::create_directories((output / name).parent_path());
        current.open(output / name, std::ios::binary);
        CHECK(current.good());
    }
    void Write(std::string_view bytes) override
    {
        if (failure == "sink-write" && name.find("summary.db") != std::string::npos) {
            throw std::runtime_error("injected write failure");
        }
        current.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        CHECK(current.good());
    }
    void Commit(std::uint64_t records) override
    {
        if (failure == "sink-commit" && name.find("symbolizer.json") != std::string::npos) {
            throw std::runtime_error("injected commit failure");
        }
        current.close();
        if (name.find("summary.db") != std::string::npos)
            CHECK(records == 1);
        committed.push_back(name);
    }
    void Abort() noexcept override
    {
        ++aborts;
        current.close();
        std::error_code ignored;
        fs::remove(output / name, ignored);
    }
};
std::shared_ptr<aclptiProfilingDataResult> Result(const std::string& mode)
{
    auto result = std::make_shared<aclptiProfilingDataResult>();
    auto context = std::make_shared<aclptiPmSamplingContext>();
    context->originalElf = std::make_shared<const std::vector<char>>(std::vector<char>{0x7f, 'E', 'L', 'F', 'o'});
    context->companionElf = std::make_shared<const std::vector<char>>(std::vector<char>{0x7f, 'E', 'L', 'F', 'a'});
    context->businessInstructions = std::make_shared<const std::map<std::uint64_t, std::uint64_t>>(
        std::map<std::uint64_t, std::uint64_t>{{0x100, 0x20}});
    context->excludedInstructions = std::make_shared<const std::set<std::uint64_t>>(std::set<std::uint64_t>{0x200});
    context->aicRuntimeEntry = 0x100000;
    result->pmSamplingContexts.emplace(7, context);
    aclptiRawDataChunk chunk{7, 3, 1, 0, true, {}};
    for (unsigned index = 0; index < 3; ++index) {
        std::array<std::uint8_t, npucompute::kPmSamplingRecordBytes> record{};
        const auto pc =
            (0x100000 + (index == 2 || mode == "probes" ? 0x200 : 0x100)) / npucompute::kPmSamplingAddressScale;
        for (unsigned byte = 0; byte < npucompute::kPmSamplingEncodedPcBytes; ++byte) {
            record[byte] = (pc >> (byte * npucompute::kPmSamplingByteBits)) & std::numeric_limits<std::uint8_t>::max();
        }
        if (mode != "zero") {
            record[npucompute::kPmSamplingStateOffset] = index == 0 ? 1 : 44;
            record[npucompute::kPmSamplingStateOffset + 1] = index == 0 ? 2 : 45;
            record[npucompute::kPmSamplingStateOffset + 2] = index == 0 ? 3 : 124;
        }
        record[npucompute::kPmSamplingActiveOffset] =
            std::numeric_limits<std::uint8_t>::max(); // Active never becomes an output state, including in zero mode.
        chunk.bytes.insert(chunk.bytes.end(), record.begin(), record.end());
    }
    if (mode == "truncated")
        chunk.bytes.pop_back();
    if (mode == "unfinished")
        chunk.isLastChunk = false;
    if (mode == "identity")
        chunk.replayId = 8;
    if (mode == "unknown")
        chunk.bytes[0] = 0xff;
    result->pcSamplingData.push_back(std::move(chunk));
    if (mode == "missing")
        result->pmSamplingContexts.clear();
    if (mode == "null-context")
        result->pmSamplingContexts[7].reset();
    if (mode == "multiple-contexts")
        result->pmSamplingContexts[8] = context;
    if (mode == "empty")
        result->pcSamplingData.clear();
    if (mode == "source-failure")
        result->status = ACLPTI_ERROR_PROFILING_FAILED;
    if (mode == "record-failure" || mode == "sampling-record-failure") {
        result->errorStats.failedRecordCount = 2;
        result->errorStats.failedRecordCountByReplay[mode == "sampling-record-failure" ? 7 : 6] = 2;
    }
    if (mode == "combo" || mode == "combo-failure" || mode == "map-failure") {
        aclptiPmuDataRow row{};
        row.coreType = ACLPTI_CORE_TYPE_AIV;
        row.totalCycles = 1000;
        row.values = {{0x422U, 100.0}, {0x57fU, 10.0}, {0x580U, 10.0}};
        result->pmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, row);
        auto& replay = result->pipelineData[6];
        replay.deviceId = 3;
        replay.format = aclptiBiuFormat::Chip6;
        replay.channels[{0, aclptiBiuCoreKind::Aic}] = {0xe00003e8U, 0xe0000000U, 0xe0000000U, 0xe0000000U,
                                                        0xf0000000U, 0xf001000aU, 0xf0000014U, 0xf0010004U};
        if (mode == "combo-failure")
            result->pcSamplingData[0].bytes.pop_back();
    }
    if (mode == "map-failure") {
        context->instructionMapError = "ambiguous business instruction alignment in kernel";
        context->businessInstructions = std::make_shared<const std::map<std::uint64_t, std::uint64_t>>();
        result->pcSamplingData.clear();
        result->status = ACLPTI_ERROR_RESULT_UNRELIABLE;
    }
    return result;
}
int DenyCreate(void*, int action, const char*, const char*, const char*, const char*)
{
    return action == npucompute::test::SQLITE_CREATE_TABLE ? SQLITE_DENY : SQLITE_OK;
}
int ConfigureSqliteFailure(sqlite3* db, char**, const sqlite3_api_routines*)
{
    return npucompute::test::SqliteSetAuthorizer(db, DenyCreate, nullptr);
}
void Child(const std::string& mode, const fs::path& root, const char* executable)
{
    const auto tools = root / "cann/tools/bisheng_compiler/bin";
    fs::create_directories(tools);
    fs::copy_file(executable, tools / "llvm-objdump");
    fs::copy_file(executable, tools / "llvm-symbolizer");
    CHECK(setenv("ASCEND_HOME_PATH", (root / "cann").c_str(), 1) == 0);
    CHECK(setenv("PM_SAMPLING_TEST_MODE", mode.c_str(), 1) == 0);
    CHECK(setenv("NPU_COMPUTE_FREQUENCY_MHZ", "1650", 1) == 0);
    CHECK(unsetenv("NPU_COMPUTE_SYSCNT_FREQUENCY_MHZ") == 0);
    npucompute::test::ResetAclPtiCallbackStub();
    npucompute::RuntimeConfig config{{"PmSampling"}, npucompute::PmuDataLevel::Block};
    if (mode == "combo" || mode == "combo-failure" || mode == "map-failure")
        config.sections = {"Memory", "Pipeline", "PmSampling"};
    Sink sink;
    sink.output = root / "received";
    sink.failure = mode;
    const auto initialStaging = StagingDirectories("/tmp");
    auto& runtime = npucompute::NpuComputeRuntime::Instance();
    int finalizedStatus = 99;
    CHECK(runtime.Initialize(config, sink, [&](int status) {
        finalizedStatus = status;
        return status;
    }) == 0);
    CHECK(callback);
    if (mode == "sqlite-failure") {
        CHECK(npucompute::test::SqliteAutoExtension(reinterpret_cast<void (*)()>(ConfigureSqliteFailure)) == SQLITE_OK);
    }
    if (mode != "no-callback") {
        CHECK(callback(Result(mode)) == ACLPTI_SUCCESS);
        if (mode == "repeat" || mode == "mixed-results") {
            CHECK(callback(Result(mode == "mixed-results" ? "truncated" : mode)) == ACLPTI_SUCCESS);
        }
    }
    runtime.Stop();
    const auto status = runtime.ShutdownAfterPtiDrain();
    const bool success = mode == "success" || mode == "zero" || mode == "repeat" || mode == "combo";
    CHECK((status == 0) == success);
    CHECK((runtime.ErrorCount() == 0) == success);
    CHECK(runtime.PmSamplingComplete() == (success || mode == "source-failure" || mode == "record-failure"));
    CHECK(finalizedStatus == status);
    CHECK(StagingDirectories("/tmp") == initialStaging);
    CHECK(StagingDirectories(root).empty());
    CHECK(callback(Result("success")) == ACLPTI_ERROR_INVALID_STATE);
    const bool partial = mode == "mixed-results";
    if (success || partial) {
        const auto output = sink.output;
        CheckDatabase(output / "summary.db", mode == "zero");
        CHECK(Read(output / "objdump.txt").find("original") != std::string::npos);
        CHECK(Read(output / "symbolizer.json").find("0x0000000000000020") != std::string::npos);
        const auto expectedRows = mode == "repeat" || mode == "combo" ? 2 : 1;
        CHECK(runtime.RowCount() == static_cast<std::uint64_t>(expectedRows));
        if (mode == "repeat") {
            const auto prefix = "collection-p" + std::to_string(getpid()) + "-0001";
            CheckDatabase(output / prefix / "summary.db", false);
        }
        if (mode == "combo") {
            CHECK(fs::exists(output / "Memory.csv"));
            CHECK(fs::exists(output / ".biu-staging/process-uds/manifest.json"));
        }
    } else {
        const bool databaseAvailable =
            mode == "source-failure" || mode == "record-failure" || mode == "tool-failure" || mode == "json-failure";
        CHECK(
            runtime.RowCount() == ((mode == "combo-failure" || mode == "map-failure" || databaseAvailable) ? 1U : 0U));
        CHECK(fs::exists(sink.output / "summary.db") == (databaseAvailable || mode == "sink-commit"));
        const bool disassemblyAvailable = mode != "no-callback" && mode != "missing" && mode != "null-context" &&
                                          mode != "multiple-contexts" && mode != "sink-begin" && mode != "sink-write" &&
                                          mode != "tool-failure";
        CHECK(fs::exists(sink.output / "objdump.txt") == disassemblyAvailable);
        const bool stackAvailable = mode == "source-failure" || mode == "record-failure" || mode == "sqlite-failure" ||
                                    mode == "sqlite-unavailable";
        CHECK(fs::exists(sink.output / "symbolizer.json") == stackAvailable);
        if (databaseAvailable)
            CheckDatabase(sink.output / "summary.db", false);
        if (mode == "combo-failure" || mode == "map-failure") {
            // Reuse the same CSV finalization stage as the CLI, after the worker
            // has reported a PmSampling failure and retained the other results.
            const auto finalized = npucompute::cli::FinalizeSummarySqlite(sink.output.string());
            CHECK(finalized.databasesUpdated == 1 && finalized.issues.empty());
            CHECK(fs::exists(sink.output / ".biu-staging/process-uds/manifest.json"));
            sqlite3* database = nullptr;
            CHECK(
                sqlite::GetApi().openV2(
                    (sink.output / "summary.db").c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
            sqlite3_stmt* statement = nullptr;
            CHECK(
                sqlite::GetApi().prepareV2(database, "SELECT count(*) FROM Memory", -1, &statement, nullptr) ==
                SQLITE_OK);
            CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW && sqlite::GetApi().columnInt64(statement, 0) > 0);
            CHECK(sqlite::GetApi().finalize(statement) == SQLITE_OK);
            CHECK(
                sqlite::GetApi().prepareV2(
                    database, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='PcSampling'", -1,
                    &statement, nullptr) == SQLITE_OK);
            CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW && sqlite::GetApi().columnInt64(statement, 0) == 0);
            CHECK(sqlite::GetApi().finalize(statement) == SQLITE_OK);
            CHECK(sqlite::GetApi().close(database) == SQLITE_OK);
        }
    }
    if (mode == "sink-write" || mode == "sink-commit")
        CHECK(sink.aborts == 1);
    CHECK(
        runtime.ReplayCount() == (mode == "no-callback" ? 0U :
                                  mode == "repeat" || mode == "mixed-results" || mode == "identity" ||
                                          mode == "multiple-contexts" || mode == "combo" || mode == "combo-failure" ||
                                          mode == "map-failure" ?
                                                          2U :
                                                          1U));
}
int Tool(int argc, char** argv)
{
    const std::string name = fs::path(argv[0]).filename();
    const std::string mode = std::getenv("PM_SAMPLING_TEST_MODE");
    if (mode == "tool-failure")
        return 9;
    const auto path = name == "llvm-objdump" ? std::string(argv[argc - 1]) : std::string(argv[1]).substr(6);
    if (Read(path) != std::string(
                          "\x7f"
                          "ELFo",
                          5))
        return 8; // Artifact tools receive the original ELF.
    if (name == "llvm-objdump") {
        std::cout << "original disassembly\n";
        return 0;
    }
    if (mode == "json-failure") {
        std::cout << "{";
        return 0;
    }
    std::string address;
    while (std::getline(std::cin, address)) {
        std::cout << "{\"Address\":\"" << address << "\",\"Symbol\":[]}";
    }
    return 0;
}
} // namespace

extern "C" aclError aclrtGetDeviceCount(std::uint32_t* count)
{
    *count = 0;
    return ACL_SUCCESS;
}
extern "C" const char* aclrtGetSocName() { return "Ascend950PR_9599"; }
extern "C" int halGetDeviceInfo(std::uint32_t, std::int32_t, std::int32_t, std::int64_t* value)
{
    *value = 1000000;
    return 0;
}
aclptiResult aclptiRegisterProfilingDataCallback(aclptiProfilingDataCallback value)
{
    callback = std::move(value);
    return ACLPTI_SUCCESS;
}
int main(int argc, char** argv)
{
    try {
        if (fs::path(argv[0]).filename() == "llvm-objdump" || fs::path(argv[0]).filename() == "llvm-symbolizer") {
            return Tool(argc, argv);
        }
        if (argc == 4) {
            Child(argv[2], argv[3], argv[0]);
            return 0;
        }
        const std::vector<std::string> modes = {
            "success",
            "zero",
            "repeat",
            "combo",
            "combo-failure",
            "map-failure",
            "no-callback",
            "empty",
            "missing",
            "null-context",
            "multiple-contexts",
            "probes",
            "truncated",
            "unfinished",
            "identity",
            "unknown",
            "source-failure",
            "record-failure",
            "sampling-record-failure",
            "sqlite-failure",
            "tool-failure",
            "json-failure",
            "sink-begin",
            "sink-write",
            "sink-commit",
            "mixed-results"};
        for (const auto& mode : modes) {
            std::string pattern = "/tmp/pm-sampling-runtime-test-XXXXXX";
            CHECK(mkdtemp(pattern.data()));
            Directory directory{pattern};
            const auto pid = fork();
            CHECK(pid >= 0);
            if (pid == 0) {
                execl(argv[0], argv[0], "--child", mode.c_str(), pattern.c_str(), nullptr);
                _exit(127);
            }
            int status = 0;
            CHECK(waitpid(pid, &status, 0) == pid);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                std::fprintf(stderr, "failed scenario: %s\n", mode.c_str());
                return 1;
            }
        }
        std::printf("%zu PmSampling worker scenarios passed\n", modes.size());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
