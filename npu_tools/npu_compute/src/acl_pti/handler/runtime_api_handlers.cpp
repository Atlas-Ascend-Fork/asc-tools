/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "runtime_api_handlers.h"
#include "securec.h"

#include "acl_pti/callback/dispatcher.h"
#include "acl_pti/profiling/replay_runtime.h"
#include "injection/injection_hook.h"

#include "aclpti/aclpti_runtime_api.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#include <utility>

namespace aclpti::handler {
namespace {
// Host kernel argument slots use eight-byte alignment in the runtime ABI.
constexpr std::size_t kHostArgumentAlignment = 8;
constexpr std::size_t kHostArgumentAlignmentMask = kHostArgumentAlignment - 1;

aclError MissingOriginalFunction(const char* apiName)
{
    ASCTOOL_ERROR(
        "[aclpti] error operation=original_lookup status=%d api=%s"
        " code=%u",
        ACL_ERROR_INTERNAL_ERROR, apiName == nullptr ? "unknown" : apiName,
        static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INTERNAL_ERROR)));
    return ACL_ERROR_INTERNAL_ERROR;
}

aclError MapProfilingResult(aclptiResult status)
{
    if (status == ACLPTI_SUCCESS) {
        return ACL_SUCCESS;
    }
    return status == ACLPTI_ERROR_RESULT_UNRELIABLE ? ACL_ERROR_INTERNAL_ERROR : ACL_ERROR_PROFILING_FAILURE;
}

struct ReplayLaunchArguments {
    // Pipeline probes leave this pointer null. SIMT PmSampling probes receive a device GM
    // buffer allocated for the duration of the replay launch.
    void* hiddenArgument = nullptr;
    std::vector<std::uint8_t> hostArguments;
    std::vector<void*> argumentPointers;
    std::vector<aclrtPlaceHolderInfo> placeholders;
};

aclError GetHiddenParameterInfo(
    aclrtFuncHandle function, std::size_t& parameterCount, std::size_t& hiddenOffset, std::size_t& hiddenSize)
{
    if (function == nullptr) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_param_metadata status=%d function=null"
            " code=%u",
            ACL_ERROR_INVALID_PARAM, static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INVALID_PARAM)));
        return ACL_ERROR_INVALID_PARAM;
    }
    const auto getCount = reinterpret_cast<aclrtFunctionGetParamCountFunc>(
        acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFunctionGetParamCount));
    const auto getInfo = reinterpret_cast<aclrtFunctionGetParamInfoFunc>(
        acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFunctionGetParamInfo));
    if (getCount == nullptr || getInfo == nullptr) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_param_api_lookup function=%p get_count=%p get_info=%p status=%d"
            " code=%u",
            function, reinterpret_cast<void*>(getCount), reinterpret_cast<void*>(getInfo), ACL_ERROR_INTERNAL_ERROR,
            static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INTERNAL_ERROR)));
        return ACL_ERROR_INTERNAL_ERROR;
    }
    const aclError countStatus = getCount(function, &parameterCount);
    if (countStatus != ACL_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_param_count function=%p api=aclrtFunctionGetParamCount status=%d "
            "param_count=%zu"
            " code=%u",
            function, countStatus, parameterCount, static_cast<unsigned>(static_cast<uint32_t>(countStatus)));
        return ACL_ERROR_FEATURE_UNSUPPORTED;
    }
    if (parameterCount == 0) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_param_count function=%p api=aclrtFunctionGetParamCount status=%d "
            "param_count=0"
            " code=%u",
            function, countStatus, static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_FEATURE_UNSUPPORTED)));
        return ACL_ERROR_FEATURE_UNSUPPORTED;
    }
    const std::size_t hiddenParameterIndex = parameterCount - 1;
    const aclError infoStatus = getInfo(function, hiddenParameterIndex, &hiddenOffset, &hiddenSize);
    if (infoStatus != ACL_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_param_info function=%p api=aclrtFunctionGetParamInfo status=%d "
            "param_count=%zu param_index=%zu param_offset=%zu param_size=%zu expected_size=%zu"
            " code=%u",
            function, infoStatus, parameterCount, hiddenParameterIndex, hiddenOffset, hiddenSize, sizeof(void*),
            static_cast<unsigned>(static_cast<uint32_t>(infoStatus)));
        return ACL_ERROR_FEATURE_UNSUPPORTED;
    }
    if (hiddenSize != sizeof(void*)) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_param_info function=%p api=aclrtFunctionGetParamInfo status=%d "
            "param_count=%zu param_index=%zu param_offset=%zu param_size=%zu expected_size=%zu"
            " code=%u",
            function, infoStatus, parameterCount, hiddenParameterIndex, hiddenOffset, hiddenSize, sizeof(void*),
            static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_FEATURE_UNSUPPORTED)));
        return ACL_ERROR_FEATURE_UNSUPPORTED;
    }
    ASCTOOL_DEBUG(
        "[aclpti] instrumented hidden parameter metadata function=%p param_count=%zu param_index=%zu param_offset=%zu "
        "param_size=%zu",
        function, parameterCount, hiddenParameterIndex, hiddenOffset, hiddenSize);
    return ACL_SUCCESS;
}

aclError BuildInstrumentedReplayArgsArray(
    aclrtFuncHandle function, void* const* originalArgs, ReplayLaunchArguments& prepared)
{
    std::size_t parameterCount = 0;
    std::size_t hiddenOffset = 0;
    std::size_t hiddenSize = 0;
    const aclError status = GetHiddenParameterInfo(function, parameterCount, hiddenOffset, hiddenSize);
    if (status != ACL_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_replay_args_array status=%d function=%p original_args=%p"
            " code=%u",
            status, function, originalArgs, static_cast<unsigned>(static_cast<uint32_t>(status)));
        return status;
    }
    std::uint32_t traceArgumentOffset = 0;
    if (!profiling::GetReplayRuntime().GetInstrumentedTraceArgumentOffset(function, traceArgumentOffset) ||
        hiddenOffset != traceArgumentOffset) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_replay_args_array function=%p hidden_offset=%zu trace_offset=%u"
            " code=%u",
            function, hiddenOffset, traceArgumentOffset, NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACL_ERROR_FEATURE_UNSUPPORTED;
    }
    const aclptiResult probeStatus =
        profiling::GetReplayRuntime().PreparePmSamplingProbeBuffer(function, &prepared.hiddenArgument);
    if (probeStatus != ACLPTI_SUCCESS) {
        return MapProfilingResult(probeStatus);
    }
    const std::size_t originalCount = parameterCount - 1;
    if (originalCount != 0 && originalArgs == nullptr) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_replay_args_array status=%d function=%p param_count=%zu "
            "original_args=null"
            " code=%u",
            ACL_ERROR_INVALID_PARAM, function, parameterCount,
            static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INVALID_PARAM)));
        return ACL_ERROR_INVALID_PARAM;
    }
    prepared.argumentPointers.reserve(parameterCount);
    for (std::size_t index = 0; index < originalCount; ++index) {
        prepared.argumentPointers.push_back(originalArgs[index]);
    }
    prepared.argumentPointers.push_back(&prepared.hiddenArgument);
    return ACL_SUCCESS;
}

aclError ResolveHostArgumentLayout(
    aclrtFuncHandle function, std::size_t argsSize, const aclrtPlaceHolderInfo* originalPlaceholders,
    std::size_t placeholderCount, std::size_t hiddenOffset, std::size_t& insertionOffset, std::size_t& paddingBytes,
    std::size_t& prefixSize);
aclError CopyHostArgumentPayload(
    const void* originalArgs, std::size_t argsSize, std::size_t insertionOffset, std::size_t hiddenOffset,
    std::size_t paddingBytes, std::size_t prefixSize, ReplayLaunchArguments& prepared);
aclError RelocateHostArgumentPlaceholders(
    aclrtFuncHandle function, const aclrtPlaceHolderInfo* originalPlaceholders, std::size_t placeholderCount,
    std::size_t insertionOffset, std::size_t paddingBytes, ReplayLaunchArguments& prepared);

aclError ValidateHostArguments(
    aclrtFuncHandle function, const void* originalArgs, std::size_t argsSize,
    const aclrtPlaceHolderInfo* originalPlaceholders, std::size_t placeholderCount)
{
    if ((argsSize != 0 && originalArgs == nullptr) || (placeholderCount != 0 && originalPlaceholders == nullptr)) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_replay_host_args status=%d function=%p args=%p args_size=%zu "
            "placeholders=%p placeholder_count=%zu"
            " code=%u",
            ACL_ERROR_INVALID_PARAM, function, originalArgs, argsSize, originalPlaceholders, placeholderCount,
            static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INVALID_PARAM)));
        return ACL_ERROR_INVALID_PARAM;
    }
    return ACL_SUCCESS;
}

aclError PrepareReplayHostProbe(
    aclrtFuncHandle function, std::size_t argsSize, ReplayLaunchArguments& prepared, std::size_t& hiddenOffset)
{
    std::uint32_t traceArgumentOffset = 0;
    if (!profiling::GetReplayRuntime().GetInstrumentedTraceArgumentOffset(function, traceArgumentOffset)) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_trace_offset function=%p args_size=%zu"
            " code=%u",
            function, argsSize, NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACL_ERROR_PROFILING_FAILURE;
    }
    const aclptiResult probeStatus =
        profiling::GetReplayRuntime().PreparePmSamplingProbeBuffer(function, &prepared.hiddenArgument);
    if (probeStatus != ACLPTI_SUCCESS) {
        return MapProfilingResult(probeStatus);
    }
    hiddenOffset = traceArgumentOffset;
    ASCTOOL_DEBUG(
        "[aclpti] instrumented host args function=%p source=binary_registry trace_offset=%u args_size=%zu", function,
        traceArgumentOffset, argsSize);
    return ACL_SUCCESS;
}

aclError BuildInstrumentedReplayHostArgs(
    aclrtFuncHandle function, const void* originalArgs, std::size_t argsSize,
    const aclrtPlaceHolderInfo* originalPlaceholders, std::size_t placeholderCount, ReplayLaunchArguments& prepared)
{
    const auto inputStatus =
        ValidateHostArguments(function, originalArgs, argsSize, originalPlaceholders, placeholderCount);
    if (inputStatus != ACL_SUCCESS) {
        return inputStatus;
    }
    std::size_t hiddenOffset = 0;
    const auto probeStatus = PrepareReplayHostProbe(function, argsSize, prepared, hiddenOffset);
    if (probeStatus != ACL_SUCCESS) {
        return probeStatus;
    }
    constexpr std::size_t hiddenSize = sizeof(void*);
    std::size_t insertionOffset = 0;
    std::size_t paddingBytes = 0;
    std::size_t prefixSize = 0;
    auto status = ResolveHostArgumentLayout(
        function, argsSize, originalPlaceholders, placeholderCount, hiddenOffset, insertionOffset, paddingBytes,
        prefixSize);
    if (status != ACL_SUCCESS) {
        return status;
    }
    status = CopyHostArgumentPayload(
        originalArgs, argsSize, insertionOffset, hiddenOffset, paddingBytes, prefixSize, prepared);
    if (status != ACL_SUCCESS) {
        return status;
    }
    status = RelocateHostArgumentPlaceholders(
        function, originalPlaceholders, placeholderCount, insertionOffset, paddingBytes, prepared);
    if (status != ACL_SUCCESS) {
        return status;
    }
    if (memcpy_s(
            prepared.hostArguments.data() + hiddenOffset, prepared.hostArguments.size() - hiddenOffset,
            &prepared.hiddenArgument, hiddenSize) != EOK) {
        return ACL_ERROR_INVALID_PARAM;
    }
    return ACL_SUCCESS;
}
aclError ResolveHostArgumentInsertion(
    aclrtFuncHandle function, std::size_t argsSize, const aclrtPlaceHolderInfo* originalPlaceholders,
    std::size_t placeholderCount, std::size_t& insertionOffset)
{
    if (placeholderCount != 0) {
        std::uint32_t lastAddressOffset = 0;
        for (std::size_t index = 0; index < placeholderCount; ++index) {
            lastAddressOffset = std::max(lastAddressOffset, originalPlaceholders[index].addrOffset);
        }
        if (lastAddressOffset > std::numeric_limits<std::uint32_t>::max() - sizeof(void*)) {
            ASCTOOL_ERROR(
                "[aclpti] error operation=instrumented_replay_host_args status=%d function=%p "
                "reason=placeholder_offset_overflow "
                "last_address_offset=%u args_size=%zu"
                " code=%u",
                ACL_ERROR_INVALID_PARAM, function, lastAddressOffset, argsSize,
                static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INVALID_PARAM)));
            return ACL_ERROR_INVALID_PARAM;
        }
        insertionOffset = static_cast<std::size_t>(lastAddressOffset) + sizeof(void*);
        if (insertionOffset > argsSize) {
            ASCTOOL_ERROR(
                "[aclpti] error operation=instrumented_replay_host_args status=%d function=%p "
                "reason=insertion_after_args "
                "insertion_offset=%zu args_size=%zu"
                " code=%u",
                ACL_ERROR_INVALID_PARAM, function, insertionOffset, argsSize,
                static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INVALID_PARAM)));
            return ACL_ERROR_INVALID_PARAM;
        }
    } else {
        if (argsSize > std::numeric_limits<std::size_t>::max() - kHostArgumentAlignmentMask) {
            ASCTOOL_ERROR(
                "[aclpti] error operation=instrumented_replay_host_args status=%d function=%p "
                "reason=alignment_overflow "
                "args_size=%zu"
                " code=%u",
                ACL_ERROR_INVALID_PARAM, function, argsSize,
                static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INVALID_PARAM)));

            return ACL_ERROR_INVALID_PARAM;
        }
        insertionOffset = (argsSize + kHostArgumentAlignmentMask) & ~kHostArgumentAlignmentMask;
    }
    return ACL_SUCCESS;
}

aclError ResolveHostArgumentLayout(
    aclrtFuncHandle function, std::size_t argsSize, const aclrtPlaceHolderInfo* originalPlaceholders,
    std::size_t placeholderCount, std::size_t hiddenOffset, std::size_t& insertionOffset, std::size_t& paddingBytes,
    std::size_t& prefixSize)
{
    constexpr std::size_t hiddenSize = sizeof(void*);
    const auto status =
        ResolveHostArgumentInsertion(function, argsSize, originalPlaceholders, placeholderCount, insertionOffset);
    if (status != ACL_SUCCESS) {
        return status;
    }
    // HostArgs may include tiling and host-memory payload after the device
    // kernel argument area. The hidden trace pointer belongs at the instrumented
    // device offset; move the tail instead of rejecting a larger HostArgs block.
    insertionOffset = std::min(insertionOffset, hiddenOffset);
    if (hiddenOffset > std::numeric_limits<std::size_t>::max() - hiddenSize) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_replay_host_args status=%d function=%p reason=hidden_offset_invalid "
            "insertion_offset=%zu hidden_offset=%zu hidden_size=%zu args_size=%zu"
            " code=%u",
            ACL_ERROR_INVALID_PARAM, function, insertionOffset, hiddenOffset, hiddenSize, argsSize,
            static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INVALID_PARAM)));
        return ACL_ERROR_INVALID_PARAM;
    }
    paddingBytes = hiddenOffset - insertionOffset;
    prefixSize = std::max(argsSize, insertionOffset);
    if (prefixSize > std::numeric_limits<std::size_t>::max() - paddingBytes ||
        prefixSize + paddingBytes > std::numeric_limits<std::size_t>::max() - hiddenSize) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=instrumented_replay_host_args status=%d function=%p "
            "reason=expanded_size_overflow "
            "prefix_size=%zu padding_bytes=%zu hidden_size=%zu"
            " code=%u",
            ACL_ERROR_INVALID_PARAM, function, prefixSize, paddingBytes, hiddenSize,
            static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INVALID_PARAM)));
        return ACL_ERROR_INVALID_PARAM;
    }
    return ACL_SUCCESS;
}

aclError CopyHostArgumentPayload(
    const void* originalArgs, std::size_t argsSize, std::size_t insertionOffset, std::size_t hiddenOffset,
    std::size_t paddingBytes, std::size_t prefixSize, ReplayLaunchArguments& prepared)
{
    constexpr std::size_t hiddenSize = sizeof(void*);
    const std::size_t expandedSize = prefixSize + paddingBytes + hiddenSize;
    prepared.hostArguments.assign(expandedSize, 0);
    if (insertionOffset != 0 && argsSize != 0) {
        if (memcpy_s(
                prepared.hostArguments.data(), prepared.hostArguments.size(), originalArgs,
                std::min(argsSize, insertionOffset)) != EOK) {
            return ACL_ERROR_INVALID_PARAM;
        }
    }
    if (argsSize > insertionOffset) {
        if (memcpy_s(
                prepared.hostArguments.data() + hiddenOffset + hiddenSize,
                prepared.hostArguments.size() - hiddenOffset - hiddenSize,
                static_cast<const std::uint8_t*>(originalArgs) + insertionOffset, argsSize - insertionOffset) != EOK) {
            return ACL_ERROR_INVALID_PARAM;
        }
    }
    return ACL_SUCCESS;
}

aclError RelocateHostArgumentPlaceholders(
    aclrtFuncHandle function, const aclrtPlaceHolderInfo* originalPlaceholders, std::size_t placeholderCount,
    std::size_t insertionOffset, std::size_t paddingBytes, ReplayLaunchArguments& prepared)
{
    constexpr std::size_t hiddenSize = sizeof(void*);
    if (placeholderCount != 0) {
        prepared.placeholders.assign(originalPlaceholders, originalPlaceholders + placeholderCount);
        for (aclrtPlaceHolderInfo& placeholder : prepared.placeholders) {
            for (auto* offset : {&placeholder.addrOffset, &placeholder.dataOffset}) {
                if (*offset < insertionOffset) {
                    continue;
                }
                if (paddingBytes > std::numeric_limits<std::uint32_t>::max() - hiddenSize ||
                    *offset > std::numeric_limits<std::uint32_t>::max() - (paddingBytes + hiddenSize)) {
                    ASCTOOL_ERROR(
                        "[aclpti] error operation=instrumented_replay_host_args status=%d function=%p "
                        "reason=placeholder_offset_overflow offset=%u padding_bytes=%zu hidden_size=%zu"
                        " code=%u",
                        ACL_ERROR_INVALID_PARAM, function, *offset, paddingBytes, hiddenSize,
                        static_cast<unsigned>(static_cast<uint32_t>(ACL_ERROR_INVALID_PARAM)));
                    return ACL_ERROR_INVALID_PARAM;
                }
                *offset += static_cast<std::uint32_t>(paddingBytes + hiddenSize);
            }
        }
    }
    return ACL_SUCCESS;
}

template <typename Function, typename Params, typename Operation>
aclError InvokeRuntimeCallback(
    aclptiCallbackId cbid, aclrtApiId apiId, const char* apiName, Params& params, Operation&& operation)
{
    callback::GetDispatcher().Dispatch(ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, ACLPTI_API_ENTER, &params, ACL_SUCCESS);
    const auto original = reinterpret_cast<Function>(acltoolGetOriginalRuntimeApi(apiId));
    aclError result;
    if (original == nullptr) {
        result = MissingOriginalFunction(apiName);
    } else {
        result = std::forward<Operation>(operation)(original);
        if (result != ACL_SUCCESS) {
            ASCTOOL_ERROR(
                "[aclpti] error operation=runtime_call status=%d api=%s"
                " code=%u",
                result, apiName, static_cast<unsigned>(static_cast<uint32_t>(result)));
        }
    }
    if (result != ACL_SUCCESS) {
        // Operation-local resources are released before shutdown and EXIT callbacks.
        (void)profiling::GetReplayRuntime().StopProfiling();
    }
    callback::GetDispatcher().Dispatch(ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, ACLPTI_API_EXIT, &params, result);
    return result;
}

template <typename Function>
bool RegisterRuntimeHandler(aclrtApiId apiId, std::int32_t (*registerCallback)(Function), Function handler)
{
    const std::int32_t result = registerCallback(handler);
    ASCTOOL_DEBUG("[aclpti] register runtime handler apiId=%d result=%d", static_cast<int>(apiId), result);
    return result == 0;
}

// Share the original launch followed by profiling replay across launch APIs.
// The member pointer reads the function after ENTER callbacks have updated params.
template <typename Function, typename Params, typename Launch>
aclError InvokeLaunch(
    aclptiCallbackId cbid, aclrtApiId apiId, const char* apiName, Params& params,
    aclrtFuncHandle Params::*functionMember, Launch launch)
{
    return InvokeRuntimeCallback<Function>(cbid, apiId, apiName, params, [&](Function original) -> aclError {
        const auto function = params.*functionMember;
        const auto invoke = [&](aclrtFuncHandle selectedFunction, bool instrumented) {
            const aclError launchStatus = launch(original, selectedFunction, instrumented);
            if (instrumented) {
                ASCTOOL_DEBUG(
                    "[aclpti] pipeline replay launch api=%s instrumented=%d original=%p selected=%p status=%d", apiName,
                    static_cast<int>(instrumented), function, selectedFunction, launchStatus);
            }
            return launchStatus;
        };
        const aclError result = invoke(function, false);
        if (result != ACL_SUCCESS) {
            return result;
        }
        const auto status = profiling::GetReplayRuntime().ReplayKernel(function, invoke, params.stream);
        ASCTOOL_DEBUG("[aclpti] launch replay api=%s result=%d", apiName, static_cast<int>(status));
        return MapProfilingResult(status);
    });
}

aclError AclrtLaunchKernelWithHostArgsHandler(
    aclrtFuncHandle funcHandle, std::uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void* hostArgs,
    std::size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, std::size_t placeHolderNum)
{
    aclptiAclrtLaunchKernelWithHostArgsParams params{funcHandle, numBlocks, stream,           cfg,
                                                     hostArgs,   argsSize,  placeHolderArray, placeHolderNum};
    return InvokeLaunch<aclrtLaunchKernelWithHostArgsFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs, ACL_RT_API_aclrtLaunchKernelWithHostArgs,
        "aclrtLaunchKernelWithHostArgs", params, &aclptiAclrtLaunchKernelWithHostArgsParams::funcHandle,
        [&params](auto launch, aclrtFuncHandle function, bool instrumented) {
            ReplayLaunchArguments prepared;
            if (instrumented) {
                const aclError status = BuildInstrumentedReplayHostArgs(
                    function, params.hostArgs, params.argsSize, params.placeHolderArray, params.placeHolderNum,
                    prepared);
                if (status != ACL_SUCCESS) {
                    return status;
                }
            }
            return launch(
                function, params.numBlocks, params.stream, params.cfg,
                instrumented ? prepared.hostArguments.data() : params.hostArgs,
                instrumented ? prepared.hostArguments.size() : params.argsSize,
                instrumented && !prepared.placeholders.empty() ? prepared.placeholders.data() : params.placeHolderArray,
                instrumented ? prepared.placeholders.size() : params.placeHolderNum);
        });
}

aclError AclrtLaunchSIMTKernelWithHostArgsHandler(
    void* func, dim3 gridDim, dim3 blockDim, std::size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    void* hostArgs, std::size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, std::size_t placeHolderNum)
{
    aclptiAclrtLaunchSIMTKernelWithHostArgsParams params{func, gridDim,  blockDim, dynUbufSize,      stream,
                                                         cfg,  hostArgs, argsSize, placeHolderArray, placeHolderNum};
    return InvokeLaunch<aclrtLaunchSIMTKernelWithHostArgsFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs, ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs,
        "aclrtLaunchSIMTKernelWithHostArgs", params, &aclptiAclrtLaunchSIMTKernelWithHostArgsParams::func,
        [&params](auto launch, aclrtFuncHandle function, bool instrumented) {
            ReplayLaunchArguments prepared;
            if (instrumented) {
                const aclError status = BuildInstrumentedReplayHostArgs(
                    function, params.hostArgs, params.argsSize, params.placeHolderArray, params.placeHolderNum,
                    prepared);
                if (status != ACL_SUCCESS) {
                    return status;
                }
            }
            return launch(
                function, params.gridDim, params.blockDim, params.dynUbufSize, params.stream, params.cfg,
                instrumented ? prepared.hostArguments.data() : params.hostArgs,
                instrumented ? prepared.hostArguments.size() : params.argsSize,
                instrumented && !prepared.placeholders.empty() ? prepared.placeholders.data() : params.placeHolderArray,
                instrumented ? prepared.placeholders.size() : params.placeHolderNum);
        });
}

aclError AclrtLaunchKernelWithArgsArrayHandler(
    void* func, std::uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void** args)
{
    aclptiAclrtLaunchKernelWithArgsArrayParams params{func, numBlocks, stream, cfg, args};
    return InvokeLaunch<aclrtLaunchKernelWithArgsArrayFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray, ACL_RT_API_aclrtLaunchKernelWithArgsArray,
        "aclrtLaunchKernelWithArgsArray", params, &aclptiAclrtLaunchKernelWithArgsArrayParams::func,
        [&params](auto launch, aclrtFuncHandle function, bool instrumented) {
            ReplayLaunchArguments prepared;
            if (instrumented) {
                const aclError status = BuildInstrumentedReplayArgsArray(function, params.args, prepared);
                if (status != ACL_SUCCESS) {
                    return status;
                }
            }
            return launch(
                function, params.numBlocks, params.stream, params.cfg,
                instrumented ? prepared.argumentPointers.data() : params.args);
        });
}

aclError AclrtLaunchSIMTKernelWithArgsArrayHandler(
    void* func, dim3 gridDim, dim3 blockDim, std::size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    void** args)
{
    aclptiAclrtLaunchSIMTKernelWithArgsArrayParams params{func, gridDim, blockDim, dynUbufSize, stream, cfg, args};
    return InvokeLaunch<aclrtLaunchSIMTKernelWithArgsArrayFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray, ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray,
        "aclrtLaunchSIMTKernelWithArgsArray", params, &aclptiAclrtLaunchSIMTKernelWithArgsArrayParams::func,
        [&params](auto launch, aclrtFuncHandle function, bool instrumented) {
            ReplayLaunchArguments prepared;
            if (instrumented) {
                const aclError status = BuildInstrumentedReplayArgsArray(function, params.args, prepared);
                if (status != ACL_SUCCESS) {
                    return status;
                }
            }
            return launch(
                function, params.gridDim, params.blockDim, params.dynUbufSize, params.stream, params.cfg,
                instrumented ? prepared.argumentPointers.data() : params.args);
        });
}

aclError AclrtMemcpyHandler(
    void* destination, std::size_t destinationSize, const void* source, std::size_t count, aclrtMemcpyKind kind)
{
    aclptiAclrtMemcpyParams params{destination, destinationSize, source, count, kind};
    return InvokeRuntimeCallback<aclrtMemcpyFunc>(
        ACLPTI_RUNTIME_CBID_aclrtMemcpy, ACL_RT_API_aclrtMemcpy, "aclrtMemcpy", params,
        [&params](aclrtMemcpyFunc function) -> aclError {
            const aclError result = function(params.dst, params.destMax, params.src, params.count, params.kind);
            if (result != ACL_SUCCESS) {
                return result;
            }
            const aclptiResult mirrorStatus = profiling::GetReplayRuntime().MirrorMemcpy(
                params.dst, params.destMax, params.src, params.count, params.kind);
            return MapProfilingResult(mirrorStatus);
        });
}

aclError AclrtBinaryLoadFromDataHandler(
    const void* data, std::size_t length, const aclrtBinaryLoadOptions* options, aclrtBinHandle* binHandle)
{
    aclptiAclrtBinaryLoadFromDataParams params{data, length, options, binHandle};
    return InvokeRuntimeCallback<aclrtBinaryLoadFromDataFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryLoadFromData, ACL_RT_API_aclrtBinaryLoadFromData, "aclrtBinaryLoadFromData",
        params, [&params](aclrtBinaryLoadFromDataFunc original) -> aclError {
            const aclError status = original(params.data, params.length, params.options, params.binHandle);
            if (status != ACL_SUCCESS) {
                return status;
            }
            return MapProfilingResult(profiling::GetReplayRuntime().RegisterBinary(
                params.data, params.length, params.options, *params.binHandle));
        });
}

aclError AclrtBinaryLoadFromFileHandler(const char* path, aclrtBinaryLoadOptions* options, aclrtBinHandle* binHandle)
{
    aclptiAclrtBinaryLoadFromFileParams params{path, options, binHandle};
    return InvokeRuntimeCallback<aclrtBinaryLoadFromFileFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryLoadFromFile, ACL_RT_API_aclrtBinaryLoadFromFile, "aclrtBinaryLoadFromFile",
        params, [&params](aclrtBinaryLoadFromFileFunc original) -> aclError {
            const aclError status = original(params.path, params.options, params.binHandle);
            if (status != ACL_SUCCESS) {
                return status;
            }
            return MapProfilingResult(
                profiling::GetReplayRuntime().RegisterBinaryFromFile(params.path, params.options, *params.binHandle));
        });
}

aclError AclrtBinaryGetFunctionHandler(
    const aclrtBinHandle binHandle, const char* kernelName, aclrtFuncHandle* funcHandle)
{
    aclptiAclrtBinaryGetFunctionParams params{binHandle, kernelName, funcHandle};
    return InvokeRuntimeCallback<aclrtBinaryGetFunctionFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction, ACL_RT_API_aclrtBinaryGetFunction, "aclrtBinaryGetFunction", params,
        [&params](aclrtBinaryGetFunctionFunc original) -> aclError {
            const aclError status = original(params.binHandle, params.kernelName, params.funcHandle);
            if (status != ACL_SUCCESS) {
                return status;
            }
            return MapProfilingResult(profiling::GetReplayRuntime().RegisterBinaryFunction(
                params.binHandle, params.kernelName, *params.funcHandle));
        });
}

aclError AclrtMallocHandler(void** devPtr, std::size_t size, aclrtMemMallocPolicy policy)
{
    aclptiAclrtMallocParams params{devPtr, size, policy};
    return InvokeRuntimeCallback<aclrtMallocFunc>(
        ACLPTI_RUNTIME_CBID_aclrtMalloc, ACL_RT_API_aclrtMalloc, "aclrtMalloc", params,
        [&params](aclrtMallocFunc mallocFunction) -> aclError {
            const aclError result = mallocFunction(params.devPtr, params.size, params.policy);
            if (result != ACL_SUCCESS) {
                return result;
            }
            const aclptiResult mirrorStatus = profiling::GetReplayRuntime().MirrorMalloc(
                params.devPtr, params.size, params.policy, ACL_RT_API_aclrtMalloc);
            return MapProfilingResult(mirrorStatus);
        });
}

aclError AclrtMallocAlign32Handler(void** devPtr, std::size_t size, aclrtMemMallocPolicy policy)
{
    aclptiAclrtMallocAlign32Params params{devPtr, size, policy};
    return InvokeRuntimeCallback<aclrtMallocAlign32Func>(
        ACLPTI_RUNTIME_CBID_aclrtMallocAlign32, ACL_RT_API_aclrtMallocAlign32, "aclrtMallocAlign32", params,
        [&params](aclrtMallocAlign32Func mallocFunction) -> aclError {
            const aclError result = mallocFunction(params.devPtr, params.size, params.policy);
            if (result != ACL_SUCCESS) {
                return result;
            }
            const aclptiResult mirrorStatus = profiling::GetReplayRuntime().MirrorMalloc(
                params.devPtr, params.size, params.policy, ACL_RT_API_aclrtMallocAlign32);
            return MapProfilingResult(mirrorStatus);
        });
}

aclError AclrtMallocWithCfgHandler(void** devPtr, std::size_t size, aclrtMemMallocPolicy policy, aclrtMallocConfig* cfg)
{
    aclptiAclrtMallocWithCfgParams params{devPtr, size, policy, cfg};
    return InvokeRuntimeCallback<aclrtMallocWithCfgFunc>(
        ACLPTI_RUNTIME_CBID_aclrtMallocWithCfg, ACL_RT_API_aclrtMallocWithCfg, "aclrtMallocWithCfg", params,
        [&params](aclrtMallocWithCfgFunc mallocFunction) -> aclError {
            const aclError result = mallocFunction(params.devPtr, params.size, params.policy, params.cfg);
            if (result != ACL_SUCCESS) {
                return result;
            }
            const aclptiResult mirrorStatus = profiling::GetReplayRuntime().MirrorMalloc(
                params.devPtr, params.size, params.policy, ACL_RT_API_aclrtMallocWithCfg, params.cfg);
            return MapProfilingResult(mirrorStatus);
        });
}

aclError AclrtMemsetHandler(void* devPtr, std::size_t maxCount, std::int32_t value, std::size_t count)
{
    aclptiAclrtMemsetParams params{devPtr, maxCount, value, count};
    return InvokeRuntimeCallback<aclrtMemsetFunc>(
        ACLPTI_RUNTIME_CBID_aclrtMemset, ACL_RT_API_aclrtMemset, "aclrtMemset", params,
        [&params](aclrtMemsetFunc function) -> aclError {
            const aclError result = function(params.devPtr, params.maxCount, params.value, params.count);
            if (result != ACL_SUCCESS) {
                return result;
            }
            const aclptiResult mirrorStatus =
                profiling::GetReplayRuntime().MirrorMemset(params.devPtr, params.maxCount, params.value, params.count);
            return MapProfilingResult(mirrorStatus);
        });
}

aclError AclrtFreeHandler(void* devPtr)
{
    aclptiAclrtFreeParams params{devPtr};
    return InvokeRuntimeCallback<aclrtFreeFunc>(
        ACLPTI_RUNTIME_CBID_aclrtFree, ACL_RT_API_aclrtFree, "aclrtFree", params,
        [&params](aclrtFreeFunc function) -> aclError {
            const aclError result = function(params.devPtr);
            if (result != ACL_SUCCESS) {
                return result;
            }
            const aclptiResult mirrorStatus = profiling::GetReplayRuntime().MirrorFree(params.devPtr);
            return MapProfilingResult(mirrorStatus);
        });
}

aclError AclrtCreateStreamHandler(aclrtStream* stream)
{
    aclptiAclrtCreateStreamParams params{stream};
    return InvokeRuntimeCallback<aclrtCreateStreamFunc>(
        ACLPTI_RUNTIME_CBID_aclrtCreateStream, ACL_RT_API_aclrtCreateStream, "aclrtCreateStream", params,
        [&params](aclrtCreateStreamFunc function) { return function(params.stream); });
}

aclError AclrtDestroyStreamHandler(aclrtStream stream)
{
    aclptiAclrtDestroyStreamParams params{stream};
    return InvokeRuntimeCallback<aclrtDestroyStreamFunc>(
        ACLPTI_RUNTIME_CBID_aclrtDestroyStream, ACL_RT_API_aclrtDestroyStream, "aclrtDestroyStream", params,
        [&params](aclrtDestroyStreamFunc function) { return function(params.stream); });
}

aclError AclrtSetDeviceHandler(std::int32_t deviceId)
{
    aclptiAclrtSetDeviceParams params{deviceId};
    return InvokeRuntimeCallback<aclrtSetDeviceFunc>(
        ACLPTI_RUNTIME_CBID_aclrtSetDevice, ACL_RT_API_aclrtSetDevice, "aclrtSetDevice", params,
        [&params](aclrtSetDeviceFunc function) { return function(params.deviceId); });
}

aclError AclrtResetDeviceHandler(std::int32_t deviceId)
{
    aclptiAclrtResetDeviceParams params{deviceId};
    return InvokeRuntimeCallback<aclrtResetDeviceFunc>(
        ACLPTI_RUNTIME_CBID_aclrtResetDevice, ACL_RT_API_aclrtResetDevice, "aclrtResetDevice", params,
        [&params](aclrtResetDeviceFunc function) { return function(params.deviceId); });
}

aclError AclrtSynchronizeStreamHandler(aclrtStream stream)
{
    aclptiAclrtSynchronizeStreamParams params{stream};
    return InvokeRuntimeCallback<aclrtSynchronizeStreamFunc>(
        ACLPTI_RUNTIME_CBID_aclrtSynchronizeStream, ACL_RT_API_aclrtSynchronizeStream, "aclrtSynchronizeStream", params,
        [&params](aclrtSynchronizeStreamFunc function) { return function(params.stream); });
}

aclError AclrtBinaryGetFunctionByEntryHandler(
    aclrtBinHandle binHandle, std::uint64_t funcEntry, aclrtFuncHandle* funcHandle)
{
    aclptiAclrtBinaryGetFunctionByEntryParams params{binHandle, funcEntry, funcHandle};
    return InvokeRuntimeCallback<aclrtBinaryGetFunctionByEntryFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry, ACL_RT_API_aclrtBinaryGetFunctionByEntry,
        "aclrtBinaryGetFunctionByEntry", params, [&params](aclrtBinaryGetFunctionByEntryFunc original) -> aclError {
            const aclError status = original(params.binHandle, params.funcEntry, params.funcHandle);
            if (status != ACL_SUCCESS) {
                return status;
            }
            return MapProfilingResult(profiling::GetReplayRuntime().RegisterBinaryFunction(
                params.binHandle, params.funcEntry, *params.funcHandle));
        });
}

aclError AclrtLaunchKernelHandler(
    aclrtFuncHandle function, std::uint32_t blockCount, const void* argsData, std::size_t argsSize, aclrtStream stream)
{
    aclptiAclrtLaunchKernelParams params{function, blockCount, argsData, argsSize, stream};
    return InvokeLaunch<aclrtLaunchKernelFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACL_RT_API_aclrtLaunchKernel, "aclrtLaunchKernel", params,
        &aclptiAclrtLaunchKernelParams::funcHandle,
        [&params](auto launch, aclrtFuncHandle function, bool instrumented) {
            ReplayLaunchArguments prepared;
            if (instrumented) {
                const aclError status =
                    BuildInstrumentedReplayHostArgs(function, params.argsData, params.argsSize, nullptr, 0, prepared);
                if (status != ACL_SUCCESS) {
                    return status;
                }
            }
            return launch(
                function, params.numBlocks, instrumented ? prepared.hostArguments.data() : params.argsData,
                instrumented ? prepared.hostArguments.size() : params.argsSize, params.stream);
        });
}

aclError AclrtGetFuncBySymbolHandler(const void* symbol, aclrtFuncHandle* funcHandle)
{
    aclptiAclrtGetFuncBySymbolParams params{symbol, funcHandle};
    return InvokeRuntimeCallback<aclrtGetFuncBySymbolFunc>(
        ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol, ACL_RT_API_aclrtGetFuncBySymbol, "aclrtGetFuncBySymbol", params,
        [&params](aclrtGetFuncBySymbolFunc original) -> aclError {
            const aclError status = original(params.symbol, params.funcHandle);
            if (status != ACL_SUCCESS) {
                return status;
            }
            return MapProfilingResult(profiling::GetReplayRuntime().RegisterSymbolFunction(*params.funcHandle));
        });
}

aclError AclrtBinaryUnLoadHandler(aclrtBinHandle binHandle)
{
    aclptiAclrtBinaryUnLoadParams params{binHandle};
    return InvokeRuntimeCallback<aclrtBinaryUnLoadFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad, ACL_RT_API_aclrtBinaryUnLoad, "aclrtBinaryUnLoad", params,
        [&params](aclrtBinaryUnLoadFunc original) -> aclError {
            auto& runtime = profiling::GetReplayRuntime();
            profiling::BinaryRegistry::UnloadContext context;
            const auto prepareStatus = runtime.PrepareBinaryUnload(params.binHandle, context);
            if (prepareStatus != ACLPTI_SUCCESS) {
                return MapProfilingResult(prepareStatus);
            }
            const aclError status = original(params.binHandle);
            if (status != ACL_SUCCESS) {
                return status; // The context releases the lock and retains the record for retry.
            }
            return MapProfilingResult(runtime.CompleteBinaryUnload(context));
        });
}

bool RegisterExecutionHandlers()
{
    return RegisterRuntimeHandler(
               ACL_RT_API_aclrtLaunchKernelWithHostArgs, acltoolRegisterAclrtLaunchKernelWithHostArgsCallbacks,
               &AclrtLaunchKernelWithHostArgsHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs, acltoolRegisterAclrtLaunchSIMTKernelWithHostArgsCallbacks,
               &AclrtLaunchSIMTKernelWithHostArgsHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtLaunchKernelWithArgsArray, acltoolRegisterAclrtLaunchKernelWithArgsArrayCallbacks,
               &AclrtLaunchKernelWithArgsArrayHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray,
               acltoolRegisterAclrtLaunchSIMTKernelWithArgsArrayCallbacks,
               &AclrtLaunchSIMTKernelWithArgsArrayHandler) &&
           RegisterRuntimeHandler(ACL_RT_API_aclrtMemcpy, acltoolRegisterAclrtMemcpyCallbacks, &AclrtMemcpyHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtBinaryLoadFromData, acltoolRegisterAclrtBinaryLoadFromDataCallbacks,
               &AclrtBinaryLoadFromDataHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtBinaryLoadFromFile, acltoolRegisterAclrtBinaryLoadFromFileCallbacks,
               &AclrtBinaryLoadFromFileHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtBinaryGetFunction, acltoolRegisterAclrtBinaryGetFunctionCallbacks,
               &AclrtBinaryGetFunctionHandler);
}

bool RegisterMemoryAndDeviceHandlers()
{
    return RegisterRuntimeHandler(ACL_RT_API_aclrtMalloc, acltoolRegisterAclrtMallocCallbacks, &AclrtMallocHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtMallocAlign32, acltoolRegisterAclrtMallocAlign32Callbacks, &AclrtMallocAlign32Handler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtMallocWithCfg, acltoolRegisterAclrtMallocWithCfgCallbacks, &AclrtMallocWithCfgHandler) &&
           RegisterRuntimeHandler(ACL_RT_API_aclrtMemset, acltoolRegisterAclrtMemsetCallbacks, &AclrtMemsetHandler) &&
           RegisterRuntimeHandler(ACL_RT_API_aclrtFree, acltoolRegisterAclrtFreeCallbacks, &AclrtFreeHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtCreateStream, acltoolRegisterAclrtCreateStreamCallbacks, &AclrtCreateStreamHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtDestroyStream, acltoolRegisterAclrtDestroyStreamCallbacks, &AclrtDestroyStreamHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtSetDevice, acltoolRegisterAclrtSetDeviceCallbacks, &AclrtSetDeviceHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtResetDevice, acltoolRegisterAclrtResetDeviceCallbacks, &AclrtResetDeviceHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtSynchronizeStream, acltoolRegisterAclrtSynchronizeStreamCallbacks,
               &AclrtSynchronizeStreamHandler);
}

bool RegisterFunctionHandlers()
{
    return RegisterRuntimeHandler(
               ACL_RT_API_aclrtBinaryGetFunctionByEntry, acltoolRegisterAclrtBinaryGetFunctionByEntryCallbacks,
               &AclrtBinaryGetFunctionByEntryHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtLaunchKernel, acltoolRegisterAclrtLaunchKernelCallbacks, &AclrtLaunchKernelHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtGetFuncBySymbol, acltoolRegisterAclrtGetFuncBySymbolCallbacks,
               &AclrtGetFuncBySymbolHandler) &&
           RegisterRuntimeHandler(
               ACL_RT_API_aclrtBinaryUnLoad, acltoolRegisterAclrtBinaryUnLoadCallbacks, &AclrtBinaryUnLoadHandler);
}

} // namespace

bool RegisterRuntimeApiHandlers()
{
    const bool registered =
        RegisterExecutionHandlers() && RegisterMemoryAndDeviceHandlers() && RegisterFunctionHandlers();
    if (!registered) {
        ASCTOOL_ERROR(
            "[aclpti] runtime handler registration failed"
            " code=%u",
            NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    ASCTOOL_DEBUG("[aclpti] runtime handler registration complete");
    return true;
}

} // namespace aclpti::handler
