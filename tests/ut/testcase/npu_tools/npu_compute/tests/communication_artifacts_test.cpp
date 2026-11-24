/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "communication/communication_collector.h"
#include "cli/ipc/artifact_receiver.h"
#include "report/collection_file_validator.h"
#include "report/collection_processor.h"
#include "report/rep_decoder.h"
#include <boost/filesystem.hpp>
#include <fstream>
#include <stdexcept>
#include <set>
#include <cstdlib>
namespace fs = boost::filesystem;
namespace ipc = npucompute::ipc;
namespace cli = npucompute::cli;
void Check(bool v)
{
    if (!v)
        throw std::runtime_error("Communication artifact expectation failed");
}
class MemorySink : public npucompute::ArtifactSink {
public:
    void Begin(const std::string& name) override
    {
        names.push_back(name);
        data.emplace_back();
    }
    void Write(std::string_view value) override { data.back().append(value); }
    void Commit(uint64_t count) override { counts.push_back(count); }
    void Abort() noexcept override {}
    std::vector<std::string> names, data;
    std::vector<uint64_t> counts;
};
void BoundedFragments()
{
    MemorySink sink;
    std::mutex mutex;
    npucompute::CommunicationCollector collector(sink, mutex);
    npucompute::CommunicationBatch batch;
    batch.records.resize(2048);
    Check(collector.Consume(batch) == ACLPTI_SUCCESS);
    Check(sink.data.size() > 1);
    uint64_t records = 0;
    for (size_t i = 0; i < sink.data.size(); ++i) {
        Check(sink.data[i].size() <= 1024U * 1024U && sink.data[i].back() == '\n');
        records += sink.counts[i];
    }
    Check(records == batch.records.size() && collector.Count() == records);
    npucompute::CommunicationBatch result;
    result.clocks.push_back({0, {}, {}, {}});
    Check(collector.Consume(result) == ACLPTI_SUCCESS);
    Check(!collector.Complete() && sink.names.back() != ".communication-staging/process-uds/manifest.json");
    Check(collector.Finish() == ACLPTI_SUCCESS && collector.Complete());
    const auto artifacts = sink.names.size();
    Check(collector.Finish() == ACLPTI_SUCCESS && sink.names.size() == artifacts);
    Check(collector.Consume(batch) == ACLPTI_ERROR_INVALID_STATE);
    Check(sink.names.back() == ".communication-staging/process-uds/manifest.json");
}
class FailingSink final : public MemorySink {
public:
    void Write(std::string_view value) override
    {
        if (fail) {
            throw std::runtime_error("injected artifact failure");
        }
        MemorySink::Write(value);
    }
    void Abort() noexcept override { ++aborted; }
    bool fail = true;
    unsigned aborted = 0;
};
void FinalizationFailures()
{
    std::mutex mutex;
    npucompute::CommunicationBatch records;
    records.records.resize(1);
    npucompute::CommunicationBatch metadata;
    metadata.clocks.push_back({0, {}, {}, {}});
    FailingSink failedRecord;
    npucompute::CommunicationCollector recordCollector(failedRecord, mutex);
    Check(recordCollector.Consume(records) == ACLPTI_ERROR_TRACE_WRITE && failedRecord.aborted == 1);
    failedRecord.fail = false;
    Check(recordCollector.Consume(metadata) == ACLPTI_ERROR_TRACE_WRITE);
    Check(recordCollector.Finish() == ACLPTI_ERROR_TRACE_INCOMPLETE && !recordCollector.Complete());
    Check(failedRecord.names.back() == ".communication-staging/process-uds/manifest.json");
    FailingSink failedManifest;
    failedManifest.fail = false;
    npucompute::CommunicationCollector manifestCollector(failedManifest, mutex);
    Check(manifestCollector.Consume(records) == ACLPTI_SUCCESS);
    Check(manifestCollector.Consume(metadata) == ACLPTI_SUCCESS);
    failedManifest.fail = true;
    Check(manifestCollector.Finish() == ACLPTI_ERROR_TRACE_WRITE && failedManifest.aborted == 1);
    const auto attempts = failedManifest.names.size();
    Check(manifestCollector.Finish() == ACLPTI_ERROR_TRACE_WRITE && failedManifest.names.size() == attempts);
    MemorySink emptySink;
    npucompute::CommunicationCollector empty(emptySink, mutex);
    Check(empty.Finish() == ACLPTI_SUCCESS && empty.Complete());
    MemorySink incompleteSink;
    npucompute::CommunicationCollector incomplete(incompleteSink, mutex);
    Check(incomplete.Consume(records) == ACLPTI_SUCCESS);
    metadata.status = ACLPTI_ERROR_TRACE_INCOMPLETE;
    metadata.errors = 1;
    Check(incomplete.Consume(metadata) == ACLPTI_SUCCESS);
    Check(incomplete.Finish() == ACLPTI_ERROR_TRACE_INCOMPLETE);
}
void InvalidFragmentNames()
{
    for (const std::string name : {"records-.jsonl", "records-a.jsonl", "records-0/1.jsonl", "records-1.json"}) {
        bool rejected = false;
        try {
            (void)ipc::KindForName(".communication-staging/process-uds/" + name);
        } catch (const ipc::Exception&) {
            rejected = true;
        }
        Check(rejected);
    }
}
void Publish(cli::ArtifactReceiver& receiver, ipc::Result& result, const std::string& name, const std::string& data)
{
    const uint64_t id = result.manifest.size() + 1;
    const auto kind = ipc::KindForName(name);
    receiver.Receive(
        {ipc::Type::DataBegin, ipc::EncodeArtifactBegin({id, kind, name, ipc::ArtifactTypeForName(name)})});
    receiver.Receive({ipc::TypeForKind(kind), ipc::EncodeArtifactChunk({id, 0, ipc::Bytes(data.begin(), data.end())})});
    receiver.Receive({ipc::Type::DataEnd, ipc::EncodeArtifactEnd({id, data.size(), 1})});
    result.manifest.push_back({id, name, data.size()});
}
int main()
{
    BoundedFragments();
    FinalizationFailures();
    InvalidFragmentNames();
    const auto base = fs::temp_directory_path() / "communication-artifact-tests" / fs::unique_path("%%%%-%%%%");
    fs::create_directories(base);
    cli::ArtifactReceiver receiver(base.string());
    ipc::Result result;
    result.sections = {"Communication"};
    for (unsigned i = 0; i < 65; ++i)
        Publish(receiver, result, ".communication-staging/process-uds/records-" + std::to_string(i) + ".jsonl", "{}\n");
    Publish(receiver, result, ".communication-staging/process-uds/manifest.json", "{}\n");
    Publish(receiver, result, "HardwareInfo.jsonl", "{}\n{}\n{}\n{}\n{}\n");
    ipc::Config config;
    config.sections = {"Communication"};
    Check(receiver.Validate(ipc::DecodeResult(ipc::EncodeResult(result)), config));
    Check(fs::exists(base / ".communication-staging/process-uds/records-0.jsonl"));
    Check(!fs::exists(base / ".biu-staging"));
    const auto report = base / "report";
    fs::create_directory(report);
    std::ofstream(report.string() + "/CommunicationTrace.json") << "[]\n";
    const std::string summary = "{\"category\":\"DeviceCommunicationSummary\",\"Scope\":\"Device\",\"Device "
                                "Id\":0,\"Compute Duration(us)\":\"NA\",\"Communication "
                                "Duration(us)\":\"NA\",\"Overlap Duration(us)\":\"NA\"}\n";
    std::ofstream(report.string() + "/app_communication_summary.jsonl") << summary;
    std::string error;
    std::vector<std::string> issues;
    std::vector<uint8_t> encoded;
    Check(cli::ValidateCollectionFile(report / "app_communication_summary.jsonl", cli::NpuRepFileType::Jsonl, &error));
    Check(cli::PackDirectoryToRep(report, &encoded, &error, &issues));
    cli::DecodedRep decoded;
    Check(cli::DecodeRep(encoded, &decoded, &error));
    std::set<std::string> names;
    for (const auto& entry : decoded.entries)
        names.insert(entry.file_name);
    Check(names.count("CommunicationTrace.json") && names.count("app_communication_summary.jsonl"));
    std::ofstream(report.string() + "/app_communication_summary.jsonl") << summary << summary;
    Check(!cli::ValidateCollectionFile(report / "app_communication_summary.jsonl", cli::NpuRepFileType::Jsonl, &error));
    std::ofstream(report.string() + "/app_communication_summary.jsonl")
        << "{\"category\":\"AppCommunicationSummary\",\"Scope\":\"All Devices\",\"Device Id\":\"All\","
           "\"Compute Duration(us)\":0,\"Communication Duration(us)\":0,\"Overlap "
           "Duration(us)\":0,\"Status\":\"complete\"}\n";
    Check(!cli::ValidateCollectionFile(report / "app_communication_summary.jsonl", cli::NpuRepFileType::Jsonl, &error));
}
