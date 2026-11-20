/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_ARTIFACT_SINK_H
#define NPU_COMPUTE_ARTIFACT_SINK_H

#include <cstdint>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>

namespace npucompute {

class ArtifactSink {
public:
    virtual ~ArtifactSink() = default;
    virtual void Begin(const std::string& name) = 0;
    virtual void Write(std::string_view bytes) = 0;
    virtual void Commit(std::uint64_t records) = 0;
    virtual void Abort() noexcept = 0;

    // A sink that streams to a shared session must publish optional files as a unit.
    virtual void PublishOptional(const std::string& name, std::string_view contents, std::uint64_t records)
    {
        Begin(name);
        try {
            Write(contents);
            Commit(records);
        } catch (...) {
            Abort();
            throw;
        }
    }
};

// Forward bounded pieces instead of retaining a whole CSV in memory.
class ArtifactStreamBuffer final : public std::streambuf {
public:
    explicit ArtifactStreamBuffer(ArtifactSink& sink) : sink_(sink) { setp(buffer_, buffer_ + sizeof(buffer_)); }

protected:
    int sync() override
    {
        try {
            if (pptr() != pbase()) {
                sink_.Write(std::string_view(pbase(), static_cast<std::size_t>(pptr() - pbase())));
                setp(buffer_, buffer_ + sizeof(buffer_));
            }
            return 0;
        } catch (...) {
            return -1;
        }
    }
    int_type overflow(int_type value) override
    {
        if (sync() != 0) {
            return traits_type::eof();
        }
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(value);
            pbump(1);
        }
        return traits_type::not_eof(value);
    }

private:
    ArtifactSink& sink_;
    char buffer_[16384];
};

} // namespace npucompute
#endif
