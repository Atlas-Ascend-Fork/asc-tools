/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "ipc/artifact_receiver.h"
#include "ipc/npu_compute_uds_client.h"
#include "compute/ipc/artifact_publisher.h"
#include "report/pm_sampling_file_validator.h"
#include "report/rep_decoder.h"
#include "report/rep_encoder.h"
#include "report/rep_directory_packer.h"
#include "report/summary_sqlite_finalizer.h"
#include "import/imported_profile_results.h"
#include "launch/staging_directory.h"
#include "pm_sampling/pm_sampling_sqlite_writer.h"
#include "sqlite_test_api.h"
#include <boost/filesystem.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sqlite = npucompute::sqlite;

namespace fs = boost::filesystem;
namespace protocol = npucompute::ipc;
namespace cli = npucompute::cli;
namespace {
#define CHECK(x)                                                                                 \
    do {                                                                                         \
        if (!(x))                                                                                \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x); \
    } while (false)
const std::array<std::string, 3> names = {"summary.db", "objdump.txt", "symbolizer.json"};
const std::string hardware = "{}\n{}\n{}\n{}\n{}\n";
struct Directory {
    fs::path path;
    Directory()
    {
        std::string value = "/tmp/pm-sampling-report-test-XXXXXX";
        CHECK(mkdtemp(value.data()));
        path = value;
    }
    ~Directory()
    {
        boost::system::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
std::string Read(const fs::path& path)
{
    std::ifstream input(path.string(), std::ios::binary);
    CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
protocol::Bytes Bytes(std::string_view input) { return {input.begin(), input.end()}; }
void Write(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path.string(), std::ios::binary);
    output.write(value.data(), value.size());
    CHECK(output.good());
}
void Reject(const std::function<void()>& operation)
{
    bool failed = false;
    try {
        operation();
    } catch (const std::exception&) {
        failed = true;
    }
    CHECK(failed);
}
void Fixture(const fs::path& directory)
{
    fs::create_directories(directory);
    npucompute::PmSamplingResult data;
    data.rows[0x10458] = {0, 215, 16320, 0, 0, 0, 0, 0};
    std::string error;
    CHECK(npucompute::WritePmSamplingSqlite((directory / names[0]).string(), data, error));
    Write(directory / names[1], "Disassembly of section .text:\n 10458:\t12345678\tMOV R0, R1\n");
    Write(directory / names[2], "[{\"Address\":\"0x0000000000010458\",\"Symbol\":[]}]\n");
}
void CheckDatabase(const fs::path& path)
{
    sqlite3* db = nullptr;
    CHECK(sqlite::GetApi().openV2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    CHECK(
        sqlite::GetApi().prepareV2(
            db, "SELECT nop_cycles, scoreboard_not_ready, others FROM PcSampling", -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite::GetApi().step(query) == SQLITE_ROW);
    CHECK(
        sqlite::GetApi().columnInt64(query, 0) == 215 && sqlite::GetApi().columnInt64(query, 1) == 16320 &&
        sqlite::GetApi().columnInt64(query, 2) == 0);
    CHECK(sqlite::GetApi().step(query) == SQLITE_DONE);
    sqlite::GetApi().finalize(query);
    CHECK(sqlite::GetApi().close(db) == SQLITE_OK);
}
void Send(cli::ArtifactReceiver& receiver, protocol::Result& result, const std::string& name, const std::string& data)
{
    const auto id = result.manifest.size() + 1;
    const auto kind = protocol::KindForName(name);
    receiver.Receive(
        {protocol::Type::DataBegin,
         protocol::EncodeArtifactBegin({id, kind, name, protocol::ArtifactTypeForName(name)})});
    for (std::size_t offset = 0; offset < data.size();) {
        const auto length = std::min<std::size_t>(37, data.size() - offset);
        receiver.Receive(
            {protocol::TypeForKind(kind),
             protocol::EncodeArtifactChunk({id, offset, Bytes(std::string_view(data).substr(offset, length))})});
        offset += length;
    }
    receiver.Receive({protocol::Type::DataEnd, protocol::EncodeArtifactEnd({id, data.size(), 1})});
    result.manifest.push_back({id, name, data.size()});
}
void RoundTrip(const std::string& mode = "success")
{
    Directory source, received, restored;
    Fixture(source.path);
    const auto bootstrap = protocol::MakeBootstrap();
    const auto child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        alarm(20);
        try {
            protocol::Listener listener(bootstrap.name);
            auto channel = listener.Accept(bootstrap.session, getppid(), getuid(), bootstrap.handshakeDeadline);
            auto hello = channel.Receive(bootstrap.handshakeDeadline);
            CHECK(hello.type == protocol::Type::Connect);
            CHECK(protocol::DecodeHello(hello.payload).config.sections == std::vector<std::string>{"PmSampling"});
            npucompute::ArtifactPublisher publisher(std::move(channel), bootstrap.handshakeDeadline);
            publisher.Begin("HardwareInfo.jsonl");
            publisher.Write(hardware);
            publisher.Commit(1);
            for (const auto& name : names) {
                if (mode == "missing-file" && name == names[2])
                    continue;
                publisher.Begin(name);
                publisher.Write(Read(source.path / name));
                publisher.Commit(0);
            }
            protocol::Result result;
            result.sections = {"PmSampling"};
            result.rowCount = 1;
            result.replayCount = 1;
            result.complete = mode != "failed-result";
            const bool accepted = publisher.Finish(result);
            _exit(accepted ? 0 : 10);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "publisher: %s\n", error.what());
            _exit(11);
        }
    }
    try {
        cli::UdsClient client;
        protocol::Config config;
        config.sections = {"PmSampling"};
        std::atomic<bool> exited{false}, cancelled{false};
        client.Run(bootstrap, child, config, received.path.string(), exited, cancelled);
        CHECK(client.Complete() == (mode == "success"));
        CHECK(client.ArtifactCount() == (mode == "missing-file" ? 3 : 4));
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    } catch (...) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        throw;
    }
    if (mode != "success")
        return; // Partial-report contents are checked below and in CLI tests.
    for (const auto& name : names)
        CHECK(Read(source.path / name) == Read(received.path / name));
    CheckDatabase(received.path / names[0]);
    protocol::Bytes encoded;
    std::string error;
    CHECK(cli::PackDirectoryToRep(received.path, &encoded, &error));
    cli::DecodedRep decoded;
    CHECK(cli::DecodeRep(encoded, &decoded, &error));
    CHECK(decoded.version == cli::NPU_REP_VERSION);
    bool sqlite = false, txt = false, json = false;
    for (const auto& entry : decoded.entries) {
        sqlite |= entry.file_name == names[0] && entry.file_type == cli::NpuRepFileType::Sqlite;
        txt |= entry.file_name == names[1] && entry.file_type == cli::NpuRepFileType::Txt;
        json |= entry.file_name == names[2] && entry.file_type == cli::NpuRepFileType::Json;
    }
    CHECK(sqlite && txt && json);
    const auto report = source.path / "result.npu-rep";
    Write(report, std::string(encoded.begin(), encoded.end()));
    std::vector<cli::ImportedProfileEntry> imported;
    CHECK(cli::ReadImportedProfileResults(report, &imported, &error));
    CHECK(cli::UnpackImportedProfileResults(imported, restored.path, &error));
    for (const auto& name : names)
        CHECK(Read(source.path / name) == Read(restored.path / name));
    CheckDatabase(restored.path / names[0]);
}
void MissingAndInvalid()
{
    Directory source;
    Fixture(source.path);
    for (const auto& omitted : names) {
        Directory output;
        cli::ArtifactReceiver receiver(output.path.string());
        protocol::Config config;
        config.sections = {"PmSampling"};
        protocol::Result result;
        result.sections = config.sections;
        result.rowCount = 1;
        Send(receiver, result, "HardwareInfo.jsonl", hardware);
        for (const auto& name : names)
            if (name != omitted)
                Send(receiver, result, name, Read(source.path / name));
        std::vector<std::string> issues;
        CHECK(!receiver.Validate(result, config, &issues) && !issues.empty());
        CHECK(issues.back().find(omitted) != std::string::npos);
        protocol::Bytes encoded;
        std::string error;
        CHECK(cli::PackDirectoryToRep(output.path, &encoded, &error));
        cli::DecodedRep decoded;
        CHECK(cli::DecodeRep(encoded, &decoded, &error));
        CHECK(decoded.entries.size() == 3);
        for (const auto& entry : decoded.entries)
            CHECK(entry.file_name != omitted);
    }
    const std::vector<std::pair<std::string, std::string>> invalid = {
        {names[0], "not sqlite"},
        {names[0], std::string("SQLite format 3\0", 16)},
        {names[1], "only labels and source lines\n"},
        {names[1], ""},
        {names[2], "{}"},
        {names[2], "["},
        {names[2], "[]"},
        {names[2],
         "[{\"Address\":\"0x0000000000010458\",\"Symbol\":[]},{\"Address\":\"0x0000000000010458\",\"Symbol\":[]}]"},
        {names[2], "[{\"Address\":\"0x0000000000010458\",\"Symbol\":[1]}]"}};
    for (const auto& item : invalid) {
        Directory output;
        for (const auto& name : names)
            Write(output.path / name, name == item.first ? item.second : Read(source.path / name));
        std::string error;
        protocol::Bytes encoded;
        CHECK(!cli::ValidatePmSamplingFile(output.path / item.first, &error));
        CHECK(error.find(item.first) != std::string::npos);
        std::vector<std::string> issues;
        CHECK(cli::PackDirectoryToRep(output.path, &encoded, &error, &issues));
        CHECK(!issues.empty());
        cli::DecodedRep decoded;
        CHECK(cli::DecodeRep(encoded, &decoded, &error));
        CHECK(decoded.entries.size() == 3); // Two good results plus collection status.
        for (const auto& entry : decoded.entries)
            CHECK(entry.file_name != item.first);
    }
    Directory received;
    cli::ArtifactReceiver receiver(received.path.string());
    protocol::Config config;
    config.sections = {"PmSampling"};
    protocol::Result result;
    result.sections = config.sections;
    result.rowCount = 1;
    Send(receiver, result, "HardwareInfo.jsonl", hardware);
    for (const auto& name : names)
        Send(receiver, result, name, Read(source.path / name));
    receiver.Validate(result, config);
    auto failed = result;
    failed.complete = false;
    CHECK(!receiver.Validate(failed, config));
    failed = result;
    failed.errorCount = 1;
    CHECK(!receiver.Validate(failed, config));
    failed = result;
    failed.rowCount = 0;
    CHECK(receiver.Validate(failed, config)); // PMU row count does not determine PmSampling validity.
    config.sections = {"Memory"};
    failed = result;
    failed.sections = config.sections;
    Reject([&] { receiver.Validate(failed, config); });
    Directory wrongGroup;
    cli::ArtifactReceiver groups(wrongGroup.path.string());
    config.sections = {"Memory", "PmSampling"};
    failed = {};
    failed.sections = config.sections;
    failed.rowCount = 1;
    Send(groups, failed, "HardwareInfo.jsonl", hardware);
    Send(groups, failed, "Memory.csv", "x\ny\n");
    Send(groups, failed, "summary.jsonl", "{}\n");
    for (const auto& name : names)
        Send(groups, failed, "collection-p1-0001/" + name, Read(source.path / name));
    CHECK(groups.Validate(failed, config)); // Separate launch groups may have different successful Sections.
    Directory csvOnly;
    sqlite3* shared = nullptr;
    CHECK(
        sqlite::GetApi().openV2(
            (csvOnly.path / "summary.db").c_str(), &shared, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
        SQLITE_OK);
    CHECK(
        sqlite::GetApi().exec(shared, "CREATE TABLE Memory(block_id INTEGER); INSERT INTO Memory VALUES(1)") ==
        SQLITE_OK);
    CHECK(sqlite::GetApi().close(shared) == SQLITE_OK);
    protocol::Bytes encoded;
    std::string error;
    CHECK(cli::ValidatePmSamplingFile(csvOnly.path / "summary.db", &error));
    CHECK(cli::PackDirectoryToRep(csvOnly.path, &encoded, &error));
    cli::DecodedRep decoded;
    CHECK(cli::DecodeRep(encoded, &decoded, &error));
    CHECK(
        decoded.entries.size() == 1 && decoded.entries[0].file_name == "summary.db" &&
        decoded.entries[0].file_type == cli::NpuRepFileType::Sqlite);
}
void SharedDatabaseRoundTrip()
{
    Directory source, report, restored;
    Fixture(source.path);
    Write(source.path / "Memory.csv", "block_id,aiv_time(us)\n1,0.5\n2,NA\n");
    const auto finalized = cli::FinalizeSummarySqlite(source.path);
    CHECK(finalized.databasesUpdated == 1 && finalized.issues.empty());
    CheckDatabase(source.path / "summary.db");
    std::string error;
    CHECK(cli::ValidatePmSamplingFile(source.path / "summary.db", &error) && error.empty());
    protocol::Bytes encoded;
    std::vector<std::string> issues;
    CHECK(cli::PackDirectoryToRep(source.path, &encoded, &error, &issues) && issues.empty());
    cli::DecodedRep decoded;
    CHECK(cli::DecodeRep(encoded, &decoded, &error));
    CHECK(decoded.version == cli::NPU_REP_VERSION && decoded.entries.size() == 4);
    Write(report.path / "shared.npu-rep", std::string(encoded.begin(), encoded.end()));
    std::vector<cli::ImportedProfileEntry> imported;
    CHECK(cli::ReadImportedProfileResults(report.path / "shared.npu-rep", &imported, &error));
    CHECK(cli::UnpackImportedProfileResults(imported, restored.path, &error));
    CHECK(Read(restored.path / "summary.db") == Read(source.path / "summary.db"));
    CHECK(Read(restored.path / "Memory.csv") == Read(source.path / "Memory.csv"));
    CheckDatabase(restored.path / "summary.db");

    sqlite3* database = nullptr;
    CHECK(
        sqlite::GetApi().openV2((restored.path / "summary.db").c_str(), &database, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    CHECK(
        sqlite::GetApi().prepareV2(
            database, "SELECT block_id, aiv_time FROM Memory ORDER BY rowid", -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite::GetApi().step(query) == SQLITE_ROW);
    CHECK(
        sqlite::GetApi().columnInt64(query, 0) == 1 && sqlite::GetApi().columnType(query, 1) == SQLITE_FLOAT &&
        sqlite::GetApi().columnDouble(query, 1) == 0.5);
    CHECK(sqlite::GetApi().step(query) == SQLITE_ROW);
    CHECK(sqlite::GetApi().columnInt64(query, 0) == 2 && sqlite::GetApi().columnType(query, 1) == SQLITE_NULL);
    CHECK(sqlite::GetApi().step(query) == SQLITE_DONE && sqlite::GetApi().finalize(query) == SQLITE_OK);
    CHECK(
        sqlite::GetApi().prepareV2(
            database, "SELECT unit FROM FieldUnitMap WHERE field_name='aiv_time'", -1, &query, nullptr) == SQLITE_OK);
    CHECK(
        sqlite::GetApi().step(query) == SQLITE_ROW &&
        std::string(reinterpret_cast<const char*>(sqlite::GetApi().columnText(query, 0))) == "us");
    CHECK(sqlite::GetApi().step(query) == SQLITE_DONE && sqlite::GetApi().finalize(query) == SQLITE_OK);
    CHECK(sqlite::GetApi().close(database) == SQLITE_OK);
}
void VersionsAndNested()
{
    std::string error;
    protocol::Bytes txt, middle, outer;
    CHECK(cli::EncodeRep({{"trace.pb", cli::NpuRepFileType::Txt, {0, 1, 2, 255}}}, &txt, &error));
    CHECK(txt[8] == 0);
    cli::DecodedRep decoded;
    CHECK(cli::DecodeRep(txt, &decoded, &error));
    CHECK(decoded.version == cli::NPU_REP_VERSION && decoded.entries[0].file_type == cli::NpuRepFileType::Txt);
    Directory report, output;
    Write(report.path / "old.npu-rep", std::string(txt.begin(), txt.end()));
    std::vector<cli::ImportedProfileEntry> imported;
    CHECK(cli::ReadImportedProfileResults(report.path / "old.npu-rep", &imported, &error));
    CHECK(cli::UnpackImportedProfileResults(imported, output.path, &error));
    CHECK(Read(output.path / "trace.pb") == std::string("\0\1\2\xff", 4));
    CHECK(cli::EncodeRep({{"a.txt", cli::NpuRepFileType::Txt, Bytes("text")}}, &txt, &error));
    CHECK(cli::EncodeRep({{"child.npu.rep", cli::NpuRepFileType::NpuRep, txt}}, &middle, &error));
    CHECK(cli::EncodeRep({{"middle.npu.rep", cli::NpuRepFileType::NpuRep, middle}}, &outer, &error));
    CHECK(txt[8] == 0 && middle[8] == 0 && outer[8] == 0);
    CHECK(cli::DecodeRep(txt, &decoded, &error) && decoded.entries[0].file_type == cli::NpuRepFileType::Txt);
    // Text entries remain compatible with the 1.0 report format.
    for (const auto* data : {&txt, &middle, &outer}) {
        const std::uint32_t version = (*data)[8] | std::uint32_t((*data)[9]) << 8 | std::uint32_t((*data)[10]) << 16 |
                                      std::uint32_t((*data)[11]) << 24;
        CHECK(version == cli::NPU_REP_VERSION);
    }
    auto future = txt;
    future[8] = 2;
    cli::RepDecodeError category;
    CHECK(!cli::DecodeRep(future, &decoded, &error, &category) && category == cli::RepDecodeError::UnsupportedVersion);
    Directory nested, recovered;
    Fixture(nested.path / "collection-p1-0001" / "nested");
    CHECK(cli::PackDirectoryToRep(nested.path, &outer, &error));
    CHECK(cli::DecodeRep(outer, &decoded, &error) && decoded.version == cli::NPU_REP_VERSION);
    const auto child = decoded.entries[0].payload;
    CHECK(cli::DecodeRep(child, &decoded, &error) && decoded.version == cli::NPU_REP_VERSION);
    Write(report.path / "nested.npu-rep", std::string(outer.begin(), outer.end()));
    CHECK(cli::ReadImportedProfileResults(report.path / "nested.npu-rep", &imported, &error));
    CHECK(cli::UnpackImportedProfileResults(imported, recovered.path, &error));
    CheckDatabase(recovered.path / "collection-p1-0001/nested" / names[0]);
}
void ProtocolRejections()
{
    for (const auto& name : names) {
        CHECK(protocol::KindForName(name) == protocol::DataKind::PmSampling);
        CHECK(protocol::TypeForKind(protocol::KindForName(name)) == protocol::Type::PmSampling);
    }
    for (const auto& name :
         {"../summary.db", "/summary.db", "collection-p1/../summary.db", ".biu-staging/process-uds/summary.db",
          "PmSampling.csv"}) {
        Reject([&] { protocol::KindForName(name); });
    }
    Directory directory;
    cli::ArtifactReceiver receiver(directory.path.string());
    receiver.Receive(
        {protocol::Type::DataBegin,
         protocol::EncodeArtifactBegin({1, protocol::DataKind::PmSampling, names[0], protocol::ArtifactType::Db})});
    Reject([&] { receiver.Receive({protocol::Type::PmSampling, protocol::EncodeArtifactChunk({1, 1, {1, 2, 3}})}); });
    CHECK(fs::is_empty(directory.path));
    Reject([&] {
        protocol::EncodeArtifactBegin({2, protocol::DataKind::PmSampling, names[0], protocol::ArtifactType::Txt});
    });
    Directory parent;
    fs::path staged;
    {
        cli::StagingDirectory staging;
        std::string error;
        CHECK(cli::StagingDirectory::Create(parent.path, &staging, &error));
        staged = staging.Path();
        Write(staged / names[1], "incomplete");
    }
    CHECK(!fs::exists(staged));
}
} // namespace
int main()
{
    try {
        RoundTrip();
        RoundTrip("failed-result");
        RoundTrip("missing-file");
        MissingAndInvalid();
        SharedDatabaseRoundTrip();
        VersionsAndNested();
        ProtocolRejections();
        std::puts("PmSampling UDS/report roundtrip and rejection checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
