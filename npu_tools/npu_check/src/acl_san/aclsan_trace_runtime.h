/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_RUNTIME_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_RUNTIME_H

#include "acl/acl_rt.h"
#include "aclsan_memory_cbdata.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aclsan {
struct DeviceInstructionDecoder;
struct ParsedTraceRecord;
namespace dav3510 {
class Dav3510RegisterStateManager;
}
} // namespace aclsan

namespace aclsan {

namespace device_runtime {
struct CallStackResult;
}

enum class TraceArgumentMode {
    HOST_ARGS,
    ARGS_ARRAY,
};

struct PreparedTraceLaunch {
    using HostInput = InternalMemoryRange;
    std::vector<HostInput> hostInputs;
    std::vector<InternalMemoryRange> readOnlyBinaryRanges;
    std::shared_ptr<void> binaryLease;
    bool instrumented = false;
    uint64_t launchId = 0;
    uint32_t blockCount = 0;
    uint32_t physicalCoreCount = 0;
    uint32_t deviceId = 0;
    // 隐藏参数 memInfo 在 kernel 连续参数布局中的字节偏移 例如[0~23为原始参数,24~31为memInfo的地址值]
    uint32_t traceArgumentOffset = 0;
    void* deviceBuffer = nullptr;
    const aclsan::DeviceInstructionDecoder* decoder = nullptr;
    std::vector<uint8_t> hostBuffer;
    // 仅 HostArgs 使用的连续参数区，ArgsArray 模式下为空。
    // 布局：[原始参数前缀][补零至 traceArgumentOffset][deviceBuffer 指针值][原始尾部数据（若有）]。
    // 隐藏指针占 sizeof(void*) 字节，保存 Device 地址，不包含 Device trace buffer 的内容。
    // 有 placeholder 时，插入点后的原始数据移至隐藏指针之后，并同步调整 placeholder.dataOffset。
    // 假设为Add(float* x, float* y, float* z);  一个指针参数对应8字节
    // 因此arguments[0~7]为x的地址值，arguments[8~15]为y的地址值，以此类推。arguments[24~31]为memInfo的地址值
    std::vector<uint8_t> arguments;
    std::vector<aclrtPlaceHolderInfo> placeholders;
};

struct TraceCollectionResult {
    size_t launchCount = 0;
    size_t completeLaunchCount = 0;
    size_t recordCount = 0;
    uint64_t droppedRecordCount = 0;

    bool Complete() const noexcept
    {
        return launchCount != 0 && completeLaunchCount == launchCount && recordCount != 0 && droppedRecordCount == 0;
    }
};

void DispatchTraceRecords(
    const std::vector<ParsedTraceRecord>& records, const DeviceInstructionDecoder& decoder,
    const std::vector<PreparedTraceLaunch::HostInput>* internalInputs = nullptr,
    dav3510::Dav3510RegisterStateManager* retainedRegisterState = nullptr,
    const std::vector<InternalMemoryRange>* readOnlyBinaryRanges = nullptr) noexcept;

void RecordTraceLoadedImage(aclrtBinHandle binary, const void* image, size_t bytes) noexcept;

void RecordTraceBinaryLoadFromData(
    aclrtBinHandle binary, bool instrumented, uint32_t traceArgumentOffset, const void* image,
    size_t imageBytes) noexcept;
void RecordTraceBinaryUnload(aclrtBinHandle binary) noexcept;
void RecordTraceBinaryFunctionLookup(
    aclrtBinHandle binary, aclrtFuncHandle function, const char* functionName) noexcept;
void RecordTraceFunctionLookup(aclrtFuncHandle function) noexcept;
aclError PrepareTraceLaunch(
    aclrtFuncHandle function, uint32_t blockCount, const void* hostArgs, size_t argsSize,
    const aclrtPlaceHolderInfo* placeholders, size_t placeholderCount, TraceArgumentMode argumentMode,
    PreparedTraceLaunch& prepared) noexcept;
void CompleteTraceLaunch(
    PreparedTraceLaunch&& prepared, const void* function, const void* stream, aclError launchResult) noexcept;
aclError MaterializeTraceHostInputs(PreparedTraceLaunch& prepared) noexcept;
TraceCollectionResult CollectTraceStream(aclrtStream stream, bool executionComplete = true) noexcept;
void ResetTraceRuntimeState() noexcept;
bool GetTraceFunctionName(aclrtFuncHandle function, std::string& functionName) noexcept;
device_runtime::CallStackResult ResolveTraceDeviceCallStack(uint64_t pc) noexcept;

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_RUNTIME_H
