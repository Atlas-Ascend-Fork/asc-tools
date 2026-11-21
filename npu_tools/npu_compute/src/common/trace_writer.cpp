/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "common/trace_writer.h"
#include "file_io.h"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace npucompute {

void TraceFileOutput::FileCloser::operator()(FILE* file) const
{
    if (file != nullptr) {
        fclose(file);
    }
}

TraceFileOutput::TraceFileOutput(const boost::filesystem::path& path, TraceFilePolicy policy, const std::string& label)
    : target_(path), policy_(policy), label_(label)
{
    if (policy_ == TraceFilePolicy::NoReplace) {
        temporary_ = path.parent_path() / boost::filesystem::unique_path(".communication-output-%%%%-%%%%-%%%%");
    } else {
        temporary_ = path;
        temporary_ += ".tmp." + std::to_string(static_cast<long long>(getpid()));
    }
    const int flags = O_WRONLY | O_CREAT | (policy_ == TraceFilePolicy::NoReplace ? O_EXCL : O_TRUNC);
    const int fd =
        asctools::security::OpenFile(temporary_.string(), flags, policy_ == TraceFilePolicy::NoReplace ? 0600 : 0666);
    if (fd < 0) {
        throw std::runtime_error("cannot create output " + path.string());
    }
    file_.reset(fdopen(fd, "w"));
    if (!file_) {
        close(fd);
        unlink(temporary_.c_str());
        throw std::runtime_error("cannot open output stream");
    }
    ownsTemporary_ = true;
}

TraceFileOutput::~TraceFileOutput()
{
    file_.reset();
    if (ownsTemporary_) {
        unlink(temporary_.c_str());
    }
}

void TraceFileOutput::Write(const std::string& value)
{
    if (!file_ || fwrite(value.data(), 1, value.size(), file_.get()) != value.size()) {
        throw std::runtime_error(label_ + " output write failed");
    }
}

void TraceFileOutput::Flush()
{
    if (!file_ || fflush(file_.get()) != 0) {
        throw std::runtime_error(label_ + " output flush failed");
    }
}

void TraceFileOutput::Close()
{
    Flush();
    FILE* file = file_.release();
    if (fclose(file) != 0) {
        throw std::runtime_error(label_ + " output close failed");
    }
}

void TraceFileOutput::Publish()
{
    if (policy_ == TraceFilePolicy::NoReplace) {
        // link is atomic and never replaces an existing target in this filesystem.
        if (link(temporary_.c_str(), target_.c_str()) != 0) {
            throw std::runtime_error("cannot publish " + label_ + " output " + target_.string());
        }
        return;
    }
    boost::system::error_code error;
    boost::filesystem::rename(temporary_, target_, error);
    if (error) {
        throw std::runtime_error(error.message());
    }
    ownsTemporary_ = false;
}

std::string TraceQuote(const std::string& value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string quoted = "\"";
    for (unsigned char character : value) {
        switch (character) {
            case '\\':
                quoted += "\\\\";
                break;
            case '"':
                quoted += "\\\"";
                break;
            case '\n':
                quoted += "\\n";
                break;
            case '\r':
                quoted += "\\r";
                break;
            case '\t':
                quoted += "\\t";
                break;
            case '\b':
                quoted += "\\b";
                break;
            case '\f':
                quoted += "\\f";
                break;
            default:
                if (character < 0x20U) {
                    quoted += "\\u00";
                    quoted += hex[character >> 4U];
                    quoted += hex[character & 0xfU];
                } else {
                    quoted += static_cast<char>(character);
                }
                break;
        }
    }
    return quoted + "\"";
}

std::string TraceNumber(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

std::string TraceNumber(long double value)
{
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<long double>::digits10) << value;
    return stream.str();
}

TraceWriter::TraceWriter(TraceFileOutput& output, TraceLayout layout) : output_(output), layout_(layout)
{
    output_.Write(
        layout_ == TraceLayout::Pipeline ?
            "{\"displayTimeUnit\":\"ns\",\"profilingType\":\"op\",\"schemaVersion\":1,\"traceEvents\":[" :
            "[");
}

void TraceWriter::Event(const std::string& event)
{
    output_.Write((first_ ? "" : (layout_ == TraceLayout::Pipeline ? "," : ",\n")) + event);
    first_ = false;
}

void TraceWriter::Duration(const TraceDurationEvent& event)
{
    const std::string identity = "\"pid\":" + event.pidJson + ",\"tid\":" + event.tidJson;
    if (layout_ == TraceLayout::Pipeline) {
        Event(
            "{\"cname\":" + TraceQuote(event.color.value_or("")) + ",\"dur\":" + event.durationUsJson + ",\"name\":" +
            TraceQuote(event.name) + ",\"ph\":\"X\"," + identity + ",\"ts\":" + event.timestampUsJson + "}");
        return;
    }
    Event(
        "{\"ph\":\"X\"," + identity + ",\"name\":" + TraceQuote(event.name) + ",\"ts\":" + event.timestampUsJson +
        ",\"dur\":" + event.durationUsJson + ",\"args\":" + event.argsJson.value_or("{}") + "}");
}

void TraceWriter::Metadata(const TraceMetadataEvent& event)
{
    Event(
        "{\"ph\":\"M\",\"name\":" + TraceQuote(event.name) + ",\"pid\":" + event.pidJson + ",\"tid\":" + event.tidJson +
        ",\"args\":{" + TraceQuote(event.field) + ":" + event.valueJson + "}}");
}

void TraceWriter::Finish()
{
    output_.Write(layout_ == TraceLayout::Pipeline ? "]}\n" : "\n]\n");
    output_.Flush();
}

} // namespace npucompute
