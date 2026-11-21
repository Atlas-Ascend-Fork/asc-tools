/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_SPOOLING_ARTIFACT_SINK_H
#define NPU_COMPUTE_SPOOLING_ARTIFACT_SINK_H
#include "artifact_sink.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <unistd.h>
namespace npucompute {
// Single-owner sink. One private unlinked file bounds memory independently of collection size.
// Publish after producer workers join and the streaming raw artifact commits.
class SpoolingArtifactSink final : public ArtifactSink {
public:
    SpoolingArtifactSink() : file_(std::tmpfile())
    {
        if (!file_)
            throw std::runtime_error("cannot create communication spool");
    }
    ~SpoolingArtifactSink() override { std::fclose(file_); }
    SpoolingArtifactSink(const SpoolingArtifactSink&) = delete;
    SpoolingArtifactSink& operator=(const SpoolingArtifactSink&) = delete;
    void Begin(const std::string& name) override
    {
        if (active_ || failed_ || published_)
            throw std::runtime_error("invalid communication spool state");
        for (const auto& entry : entries_)
            if (entry.name == name)
                throw std::runtime_error("duplicate communication artifact");
        const auto offset = ftello(file_);
        if (offset < 0)
            throw std::runtime_error("cannot seek communication spool");
        current_ = {name, offset};
        active_ = true;
    }
    void Write(std::string_view bytes) override
    {
        if (!active_ || std::fwrite(bytes.data(), sizeof(char), bytes.size(), file_) != bytes.size()) {
            failed_ = true;
            throw std::runtime_error("cannot write communication spool");
        }
        current_.bytes += bytes.size();
    }
    void Commit(uint64_t records) override
    {
        if (!active_ || std::fflush(file_) != 0) {
            failed_ = true;
            throw std::runtime_error("cannot commit communication spool");
        }
        current_.records = records;
        entries_.push_back(current_);
        active_ = false;
    }
    void Abort() noexcept override
    {
        failed_ = true;
        active_ = false;
    }
    void Publish(ArtifactSink& sink)
    {
        if (active_ || failed_ || published_)
            throw std::runtime_error("incomplete communication spool");
        constexpr std::size_t copyBufferBytes = 64U * 1024U;
        std::array<char, copyBufferBytes> buffer{};
        for (const auto& entry : entries_) {
            if (fseeko(file_, entry.offset, SEEK_SET) != 0)
                throw std::runtime_error("cannot read communication spool");
            sink.Begin(entry.name);
            try {
                uint64_t remaining = entry.bytes;
                while (remaining != 0) {
                    const auto size = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                    if (std::fread(buffer.data(), sizeof(char), size, file_) != size)
                        throw std::runtime_error("truncated communication spool");
                    sink.Write(std::string_view(buffer.data(), size));
                    remaining -= size;
                }
                sink.Commit(entry.records);
            } catch (...) {
                sink.Abort();
                throw;
            }
        }
        published_ = true;
        (void)ftruncate(fileno(file_), 0);
    }

private:
    struct Entry {
        std::string name;
        off_t offset;
        uint64_t bytes = 0, records = 0;
    };
    FILE* file_;
    Entry current_{};
    std::vector<Entry> entries_;
    bool active_ = false, failed_ = false, published_ = false;
};
} // namespace npucompute
#endif
