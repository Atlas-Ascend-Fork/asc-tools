/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/summary_sqlite_finalizer.h"
#include <string_view>
#include "report/csv_reader.h"
#include "report/csv_sqlite_writer.h"
#include "shared/sqlite_runtime_loader.h"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace npucompute::cli {
namespace {
namespace fs = boost::filesystem;
constexpr std::string_view kTemporarySuffix = ".tmp";

bool IsTemporary(const std::string& name)
{
    return name.find(".tmp.") != std::string::npos ||
           (name.size() >= kTemporarySuffix.size() &&
            name.compare(name.size() - kTemporarySuffix.size(), kTemporarySuffix.size(), kTemporarySuffix) == 0);
}

fs::file_status Inspect(const fs::path& path)
{
    boost::system::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error && status.type() != fs::file_not_found)
        throw std::runtime_error("cannot inspect '" + path.string() + "': " + error.message());
    return status;
}

struct CsvInput {
    fs::path path;
    CsvSchema schema;
};

std::vector<CsvInput> AnalyzeInputs(const std::vector<fs::path>& paths, std::vector<std::string>& issues)
{
    std::vector<CsvInput> inputs;
    for (const auto& path : paths) {
        try {
            auto schema = AnalyzeCsv(path.string());
            if (schema.rowCount != 0)
                inputs.push_back({path, std::move(schema)});
        } catch (const std::exception& error) {
            issues.push_back(std::string(error.what()) + "; skipped this CSV table; other valid results are retained");
        }
    }
    return inputs;
}

bool HasOriginal(const fs::path& path)
{
    const auto status = Inspect(path);
    if (fs::exists(status) && !fs::is_regular_file(status))
        throw std::runtime_error("existing database is not a regular file");
    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
        const auto sidecar = fs::path(path.string() + suffix);
        if (fs::exists(Inspect(sidecar)))
            throw std::runtime_error(
                "SQLite sidecar '" + sidecar.string() +
                "' remains; cannot safely create or copy the main database while auxiliary SQLite files remain");
    }
    if (!fs::exists(status))
        return false;
    std::ifstream input(path.string(), std::ios::binary);
    std::array<char, sizeof(sqlite::kDatabaseHeader)> header{};
    input.read(header.data(), header.size());
    if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
        std::memcmp(header.data(), sqlite::kDatabaseHeader, header.size()) != 0)
        throw std::runtime_error("existing database has an invalid SQLite header or cannot be read");
    return true;
}

class WorkingDatabase {
public:
    ~WorkingDatabase()
    {
        if (handle)
            sqlite::GetApi().closeV2(handle);
        if (!directory.empty()) {
            boost::system::error_code ignored;
            fs::remove_all(directory, ignored);
        }
    }
    void Check(int status) const
    {
        if (status != SQLITE_OK)
            throw std::runtime_error(
                handle ? sqlite::GetApi().errmsg(handle) : "SQLite could not allocate a connection");
    }
    void Exec(const char* sql) const { Check(sqlite::GetApi().exec(handle, sql)); }
    void Close()
    {
        Check(sqlite::GetApi().close(handle));
        handle = nullptr;
    }
    void Cleanup(std::vector<std::string>& issues)
    {
        if (handle) {
            const auto& api = sqlite::GetApi();
            if (transaction && api.exec(handle, "ROLLBACK") != SQLITE_OK)
                issues.push_back(
                    "Cannot roll back working SQLite database '" + path.string() + "': " + api.errmsg(handle));
            if (api.closeV2(handle) != SQLITE_OK)
                issues.push_back(
                    "Cannot close discarded working SQLite database '" + path.string() + "': " + api.errmsg(handle));
            else
                handle = nullptr;
        }
        if (!directory.empty()) {
            boost::system::error_code error;
            fs::remove_all(directory, error);
            if (error)
                issues.push_back(
                    "Cannot remove temporary SQLite directory '" + directory.string() + "': " + error.message() +
                    "; temporary files are excluded from the report");
            else
                directory.clear();
        }
    }
    fs::path directory;
    fs::path path;
    sqlite3* handle = nullptr;
    bool transaction = false;
};

void UseDeleteJournal(WorkingDatabase& database)
{
    const auto& api = sqlite::GetApi();
    sqlite3_stmt* statement = nullptr;
    database.Check(
        api.prepareV2(database.handle, "PRAGMA journal_mode=DELETE", sqlite::kNullTerminatedSql, &statement, nullptr));
    const auto row = api.step(statement);
    const auto* mode = row == SQLITE_ROW ? api.columnText(statement, 0) : nullptr;
    const bool selected = mode && std::strcmp(reinterpret_cast<const char*>(mode), "delete") == 0;
    const auto done = selected ? api.step(statement) : row;
    const auto finalized = api.finalize(statement);
    if (!selected || done != SQLITE_DONE)
        throw std::runtime_error("cannot select SQLite DELETE journal mode for a self-contained database file");
    database.Check(finalized);
}

void FinalizeDirectory(
    const fs::path& directory, const std::vector<fs::path>& paths, SummarySqliteFinalizationResult& result)
{
    const auto inputs = AnalyzeInputs(paths, result.issues);
    if (inputs.empty())
        return;
    const auto target = directory / "summary.db";
    WorkingDatabase database;
    std::string stage = "inspect original database";
    try {
        const bool original = HasOriginal(target);
        stage = "create temporary database directory";
        const auto temporary = directory / fs::unique_path(".summary.tmp.%%%%-%%%%-%%%%");
        if (!fs::create_directory(temporary))
            throw std::runtime_error("temporary directory already exists");
        database.directory = temporary;
        database.path = temporary / "summary.db";
        if (original) {
            stage = "copy original database";
            fs::copy_file(target, database.path);
        }
        stage = "load SQLite runtime";
        const auto& api = sqlite::GetApi();
        stage = "open working database";
        database.Check(api.openV2(
            database.path.string().c_str(), &database.handle,
            SQLITE_OPEN_READWRITE | (original ? 0 : SQLITE_OPEN_CREATE), nullptr));
        stage = "read existing database schema";
        database.Exec("SELECT name FROM sqlite_master LIMIT 1");
        stage = "configure working database journal";
        UseDeleteJournal(database);
        stage = "begin database transaction";
        database.Exec("BEGIN");
        database.transaction = true;
        std::size_t written = 0;
        for (const auto& input : inputs) {
            stage = "write CSV table '" + input.schema.tableName + "' from '" + input.path.string() + "'";
            const auto imported = WriteCsvTable(database.handle, input.path.string(), input.schema);
            if (!imported.error.empty())
                result.issues.push_back(imported.error);
            if (!imported.databaseUsable)
                throw std::runtime_error("CSV table rollback failed; discard all updates in this working database");
            if (imported.written)
                ++written;
        }
        if (written != 0) {
            stage = "commit database transaction";
            database.Exec("COMMIT");
            database.transaction = false;
            stage = "close working database";
            database.Close();
            stage = "publish database";
            fs::rename(database.path, target);
            ++result.databasesUpdated;
        }
    } catch (const std::exception& error) {
        result.issues.push_back(
            "Cannot update SQLite database '" + target.string() + "' during " + stage + ": " + error.what() +
            "; discarded this database update; any original database and other valid files are retained");
    }
    database.Cleanup(result.issues);
}

void CollectDirectory(
    const fs::path& directory, std::vector<fs::path>& csvs, std::vector<fs::path>& children,
    std::vector<std::string>& issues)
{
    if (!fs::is_directory(Inspect(directory)))
        throw std::runtime_error("collection path is not a regular directory");
    for (fs::directory_iterator iterator(directory), end; iterator != end; ++iterator) {
        const auto& path = iterator->path();
        const auto name = path.filename().string();
        if (name == ".biu-staging" || IsTemporary(name))
            continue;
        try {
            const auto status = Inspect(path);
            if (fs::is_symlink(status)) {
                issues.push_back(
                    "Cannot import SQLite data through symbolic link '" + path.string() + "'; skipped this path");
            } else if (fs::is_directory(status))
                children.push_back(path);
            else if (fs::is_regular_file(status) && path.extension() == ".csv")
                csvs.push_back(path);
        } catch (const std::exception& error) {
            issues.push_back("Cannot inspect SQLite input '" + path.string() + "': " + error.what());
        }
    }
}
} // namespace

SummarySqliteFinalizationResult FinalizeSummarySqlite(const fs::path& collectionDirectory)
{
    SummarySqliteFinalizationResult result;
    std::vector<fs::path> pending{collectionDirectory};
    while (!pending.empty()) {
        const auto directory = pending.back();
        pending.pop_back();
        std::vector<fs::path> csvs;
        std::vector<fs::path> children;
        try {
            CollectDirectory(directory, csvs, children, result.issues);
        } catch (const std::exception& error) {
            result.issues.push_back(
                "Cannot read SQLite result directory '" + directory.string() + "': " + error.what() +
                "; other result directories are retained");
        }
        std::sort(csvs.begin(), csvs.end());
        std::sort(children.rbegin(), children.rend());
        pending.insert(pending.end(), children.begin(), children.end());
        FinalizeDirectory(directory, csvs, result);
    }
    return result;
}
} // namespace npucompute::cli
