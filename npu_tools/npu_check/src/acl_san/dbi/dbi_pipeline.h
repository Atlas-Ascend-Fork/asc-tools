/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_DBI_PIPELINE_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_DBI_PIPELINE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aclsan {

enum class ProbeGroup : uint8_t { Mte1, Mte2, Mte3, Fixpipe, Scalar, Sync, Matrix, Vector, Boundary, Simt };

enum class ProbeCoreType : uint8_t { Unknown, Aic, Aiv, Mix };

enum ProbeGroupMask : uint32_t {
    PROBE_GROUP_MTE1 = 1U << 0U,
    PROBE_GROUP_MTE2 = 1U << 1U,
    PROBE_GROUP_MTE3 = 1U << 2U,
    PROBE_GROUP_FIXPIPE = 1U << 3U,
    PROBE_GROUP_SYNC = 1U << 4U,
    PROBE_GROUP_SCALAR = 1U << 5U,
    PROBE_GROUP_MATRIX = 1U << 6U,
    PROBE_GROUP_VECTOR = 1U << 7U,
    // always-on 的 kernel 边界 marker 组（397 before kernel end / 398 before kernel stack frame）。
    PROBE_GROUP_BOUNDARY = 1U << 8U,
    PROBE_GROUP_SIMT = 1U << 9U,
    PROBE_GROUP_ALL = PROBE_GROUP_MTE1 | PROBE_GROUP_MTE2 | PROBE_GROUP_MTE3 | PROBE_GROUP_FIXPIPE | PROBE_GROUP_SYNC |
                      PROBE_GROUP_SCALAR | PROBE_GROUP_MATRIX | PROBE_GROUP_VECTOR | PROBE_GROUP_BOUNDARY |
                      PROBE_GROUP_SIMT,
};

struct ToolchainPaths {
    std::string bisheng;
    std::string bishengTune;
    std::string ldLld;
    std::string llvmObjdump;

    bool Complete() const;
};

struct DbiRequest {
    std::string inputKernel;
    std::string outputKernel;
    std::string arch;
    // DBI 在 binary load 阶段为隐藏的、每次 launch 注入的 trace buffer 指针预留的 kernel 参数偏移。
    uint32_t traceArgumentOffset = 0;
    std::vector<ProbeGroup> probeGroups;
    std::string toolchainRoot;
    std::string workDirectory;
    std::string cacheDirectory;
    bool strict = false;
    bool keepTemp = false;
    std::vector<std::string> extraTuneArgs;
    std::optional<uint64_t> tilingKey = std::nullopt;
    ProbeCoreType coreType = ProbeCoreType::Unknown;
    bool useCompleteProbeSet = false;
};

struct DbiResult {
    bool success = false;
    std::string patchedPath;
    std::string stage;
    std::string diagnostic;
    // 已写入 patched binary，并由 launch hook 用于填充隐藏 trace buffer 指针的参数偏移。
    uint32_t traceArgumentOffset = 0;
};

std::vector<ProbeGroup> NormalizeProbeGroups(const std::vector<ProbeGroup>& groups);
std::vector<ProbeGroup> ProbeGroupsFromMask(uint32_t mask);
std::string ProbeGroupName(ProbeGroup group);
std::string ProbeCoreTypeName(ProbeCoreType coreType);
ProbeCoreType DetectProbeCoreType(const std::string& kernelSymbols, const std::string& selectedKernelSymbol);
std::vector<ProbeGroup> ResolveProbeGroups(
    const std::vector<ProbeGroup>& requestedGroups, ProbeCoreType coreType, bool useCompleteProbeSet);
std::string ValidateRequest(const DbiRequest& request);
ToolchainPaths ResolveToolchain(const std::string& cannRoot);
std::string CannRootFromRuntimeLibrary(const std::string& runtimeLibrary);
std::string MakeCacheKey(
    const std::string& arch, const std::vector<ProbeGroup>& groups, const std::string& objectIdentity,
    ProbeCoreType coreType = ProbeCoreType::Mix);
DbiResult RunDbiPipeline(const DbiRequest& request);

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_DBI_PIPELINE_H
