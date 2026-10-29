/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "hardware/hardware_info_host.h"

#include <boost/filesystem.hpp>

#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <cerrno>
#include <cstring>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <unistd.h>

namespace npucompute {
namespace {
constexpr std::uint32_t DECIMAL_RADIX = 10;

constexpr uint64_t BYTES_PER_MB = 1024ULL * 1024ULL;
constexpr uint64_t BYTES_PER_GB = BYTES_PER_MB * 1024ULL;
constexpr uint32_t MAXIMUM_CPU_ID = 1024U * 1024U;

void Diagnose(const DiagnosticSink* diagnostics, const std::string& message)
{
    if (diagnostics != nullptr && *diagnostics) {
        (*diagnostics)(message);
    }
}

std::string_view Trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

template <typename Integer>
bool ParseInteger(std::string_view text, Integer* value)
{
    const std::string_view trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    const bool negative = trimmed.front() == '-';
    const std::size_t begin = negative ? 1U : 0U;
    if ((negative && !std::numeric_limits<Integer>::is_signed) || begin == trimmed.size()) {
        return false;
    }
    // CPU/package IDs must be non-negative; preserve signed negative zero.
    const uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<Integer>::max());
    uint64_t parsedValue = 0;
    for (std::size_t index = begin; index < trimmed.size(); ++index) {
        const char character = trimmed[index];
        if (character < '0' || character > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(static_cast<unsigned>(character) - static_cast<unsigned>('0'));
        if (parsedValue > (maximum - digit) / DECIMAL_RADIX) {
            return false;
        }
        parsedValue = parsedValue * DECIMAL_RADIX + digit;
    }
    if (negative && parsedValue != 0) {
        return false;
    }
    *value = static_cast<Integer>(parsedValue);
    return true;
}

bool ReadFile(const boost::filesystem::path& path, std::string* content)
{
    std::ifstream input(path.string());
    if (!input.is_open()) {
        return false;
    }
    content->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

bool ParseOnlineCpuList(std::string_view text, std::vector<uint32_t>* cpuIds)
{
    cpuIds->clear();
    std::set<uint32_t> uniqueIds;
    text = Trim(text);
    if (text.empty()) {
        return false;
    }

    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t separator = text.find(',', offset);
        const std::size_t tokenLength = separator == std::string_view::npos ? text.size() - offset : separator - offset;
        const std::string_view token = Trim(text.substr(offset, tokenLength));
        if (token.empty()) {
            return false;
        }

        const std::size_t rangeSeparator = token.find('-');
        uint32_t first = 0;
        uint32_t last = 0;
        if (rangeSeparator == std::string_view::npos) {
            if (!ParseInteger(token, &first)) {
                return false;
            }
            last = first;
        } else {
            if (token.find('-', rangeSeparator + 1) != std::string_view::npos ||
                !ParseInteger(token.substr(0, rangeSeparator), &first) ||
                !ParseInteger(token.substr(rangeSeparator + 1), &last) || first > last) {
                return false;
            }
        }
        if (last > MAXIMUM_CPU_ID) {
            return false;
        }
        for (uint32_t cpuId = first; cpuId <= last; ++cpuId) {
            uniqueIds.insert(cpuId);
        }

        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1;
    }

    cpuIds->assign(uniqueIds.begin(), uniqueIds.end());
    return !cpuIds->empty();
}

void CollectPhysicalCpuCount(const HostInfoCollectionOptions& options, HostInfo* result, DiagnosticSink* diagnostics)
{
    const boost::filesystem::path onlinePath = options.cpuTopologyRoot / "online";
    std::string online;
    if (!ReadFile(onlinePath, &online)) {
        Diagnose(diagnostics, "read CPU online list failed: " + onlinePath.string());
        return;
    }

    std::vector<uint32_t> cpuIds;
    if (!ParseOnlineCpuList(online, &cpuIds)) {
        Diagnose(diagnostics, "parse CPU online list failed: " + onlinePath.string());
        return;
    }

    std::set<std::int64_t> packageIds;
    for (const uint32_t cpuId : cpuIds) {
        const boost::filesystem::path packagePath =
            options.cpuTopologyRoot / ("cpu" + std::to_string(cpuId)) / "topology/physical_package_id";
        std::string packageText;
        if (!ReadFile(packagePath, &packageText)) {
            Diagnose(diagnostics, "read CPU package ID failed: " + packagePath.string());
            continue;
        }
        std::int64_t packageId = -1;
        if (!ParseInteger(packageText, &packageId) || packageId < 0) {
            Diagnose(diagnostics, "parse CPU package ID failed: " + packagePath.string());
            continue;
        }
        packageIds.insert(packageId);
    }

    if (packageIds.size() > std::numeric_limits<uint32_t>::max()) {
        Diagnose(diagnostics, "CPU package count exceeds supported range");
        return;
    }
    result->cpuPhysicalCount = static_cast<uint32_t>(packageIds.size());
}

void CollectLogicalCpuCount(HostInfo* result, DiagnosticSink* diagnostics)
{
    const long count = ::sysconf(_SC_NPROCESSORS_CONF);
    if (count <= 0 || static_cast<unsigned long>(count) > std::numeric_limits<uint32_t>::max()) {
        Diagnose(diagnostics, "sysconf(_SC_NPROCESSORS_CONF) failed");
        return;
    }
    result->cpuLogicalCount = static_cast<uint32_t>(count);
}

void CollectMemoryTotal(HostInfo* result, DiagnosticSink* diagnostics)
{
    struct sysinfo information {};
    if (::sysinfo(&information) != 0) {
        Diagnose(diagnostics, "sysinfo failed: " + std::string(std::strerror(errno)));
        return;
    }

    const uint64_t totalRam = information.totalram;
    const uint64_t memoryUnit = information.mem_unit;
    if (memoryUnit != 0 && totalRam > std::numeric_limits<uint64_t>::max() / memoryUnit) {
        Diagnose(diagnostics, "sysinfo total memory exceeds supported range");
        return;
    }
    const uint64_t bytes = totalRam * memoryUnit;
    result->memoryTotalSizeMb = static_cast<double>(bytes) / static_cast<double>(BYTES_PER_MB);
}

void CollectDiskTotal(const boost::filesystem::path& outputDirectory, HostInfo* result, DiagnosticSink* diagnostics)
{
    struct statvfs information {};
    if (::statvfs(outputDirectory.c_str(), &information) != 0) {
        Diagnose(diagnostics, "statvfs failed for " + outputDirectory.string() + ": " + std::strerror(errno));
        return;
    }

    const uint64_t blocks = information.f_blocks;
    const uint64_t fragmentSize = information.f_frsize;
    if (fragmentSize != 0 && blocks > std::numeric_limits<uint64_t>::max() / fragmentSize) {
        Diagnose(diagnostics, "statvfs total size exceeds supported range");
        return;
    }
    const uint64_t bytes = blocks * fragmentSize;
    result->diskTotalSizeGb = static_cast<double>(bytes) / static_cast<double>(BYTES_PER_GB);
}

} // namespace

bool CollectHostInfo(
    const boost::filesystem::path& outputDirectory, HostInfo* result, DiagnosticSink* diagnostics,
    const HostInfoCollectionOptions& options)
{
    if (result == nullptr) {
        Diagnose(diagnostics, "HostInfo result is null");
        return false;
    }

    *result = {};
    CollectPhysicalCpuCount(options, result, diagnostics);
    CollectLogicalCpuCount(result, diagnostics);
    CollectMemoryTotal(result, diagnostics);
    CollectDiskTotal(outputDirectory, result, diagnostics);
    return true;
}

} // namespace npucompute
