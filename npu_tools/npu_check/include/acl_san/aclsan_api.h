/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_API_H
#define NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_API_H

#include <stdint.h>
#include "acl_san/aclsan_cbdata.h"

#define ACLSAN_EXPORT __attribute__((visibility("default")))
#define ACLSAN_API_VERSION 1u
#define ACLSAN_CALL_STACK_MAX_DEPTH 16U
#define ACLSAN_FUNCTION_NAME_MAX_BYTES 4096U
#define ACLSAN_FILE_NAME_MAX_BYTES 4096U

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t AclsanStatus;
typedef enum AclsanStatusValue {
    ACLSAN_STATUS_SUCCESS = 0,
    ACLSAN_STATUS_ERROR_INVALID_VALUE = 1,       // 未用到
    ACLSAN_STATUS_ERROR_NOT_INITIALIZED = 2,     // 未用到
    ACLSAN_STATUS_ERROR_ALREADY_INITIALIZED = 3, // 未用到
    ACLSAN_STATUS_ERROR_VERSION_MISMATCH = 4,    // 未用到
    ACLSAN_STATUS_ERROR_NOT_SUPPORTED = 5,
    ACLSAN_STATUS_ERROR_OUT_OF_MEMORY = 6,
    ACLSAN_STATUS_ERROR_IO = 7, // 未用到
    ACLSAN_STATUS_ERROR_RUNTIME = 8,
    ACLSAN_STATUS_ERROR_PATCH_FAILED = 9, // 未用到
    ACLSAN_STATUS_ERROR_REENTRANT = 10,   // 未用到
    ACLSAN_STATUS_ERROR_NOT_FOUND = 11,   // 未用到
    ACLSAN_STATUS_ERROR_MAX_LIMIT_REACHED = 12,
    ACLSAN_STATUS_ERROR_INVALID_PARAMETER = 13,
    ACLSAN_STATUS_ERROR_INVALID_STATE = 14,
    ACLSAN_STATUS_ERROR_ALREADY_SUBSCRIBED = 15,
    ACLSAN_STATUS_ERROR_INJECTION_FAILED = 16,
    ACLSAN_STATUS_ERROR_INTERNAL = 17
} AclsanStatusValue;

typedef enum AclsanCallStackFlags {
    ACLSAN_CALL_STACK_FLAG_NONE = 0,
    ACLSAN_CALL_STACK_FLAG_TRUNCATED = 1 // 表明当前调用栈大于ACLSAN_CALL_STACK_MAX_DEPTH导致被截断
} AclsanCallStackFlags;

typedef struct AclsanDeviceCallStackFrame {
    uint32_t line;
    uint32_t column;
    uint32_t inlineDepth; // 这一帧在静态 DWARF inline 调用链中的层级
    char functionName[ACLSAN_FUNCTION_NAME_MAX_BYTES];
    char fileName[ACLSAN_FILE_NAME_MAX_BYTES];
} AclsanDeviceCallStackFrame;

typedef struct AclsanDeviceCallStack {
    uint64_t binaryId; // 进程内本次 binary load 的非零标识；无活动 binary 时为 0   TODO: 后续看怎么维护
    uint64_t pc;                                                    // 本次查询的 Device PC
    uint32_t depth;                                                 // frames 中有效元素的数量
    uint32_t flags;                                                 // AclsanCallStackFlags
    AclsanDeviceCallStackFrame frames[ACLSAN_CALL_STACK_MAX_DEPTH]; // 有效范围为 [0, depth)
} AclsanDeviceCallStack;

typedef enum AclsanCallbackDomain {
    ACLSAN_CB_DOMAIN_INVALID = 0,
    ACLSAN_CB_DOMAIN_RESOURCE = 1,
    ACLSAN_CB_DOMAIN_SYNCHRONIZE = 2,
    ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION = 3,
    ACLSAN_CB_DOMAIN_MEMORY = 4,
    ACLSAN_CB_DOMAIN_BINARY = 5,
    ACLSAN_CB_DOMAIN_PATCH = 6,
    ACLSAN_CB_DOMAIN_LAUNCH = 7,
    ACLSAN_CB_DOMAIN_REPORT = 8,
    ACLSAN_CB_DOMAIN_ERROR = 9,
    ACLSAN_CB_DOMAIN_DEVICE_BARRIER = 10,
} AclsanCallbackDomain;

typedef enum AclsanCallbackIdResource {
    ACLSAN_CBID_RESOURCE_MEMORY_ALLOC = 1, // 需要用到
    ACLSAN_CBID_RESOURCE_MEMORY_FREE = 2,  // 需要用到
    ACLSAN_CBID_RESOURCE_MODULE_LOAD = 3,
    ACLSAN_CBID_RESOURCE_MODULE_UNLOAD = 4,
    ACLSAN_CBID_RESOURCE_FUNCTION_GET = 5,
    ACLSAN_CBID_RESOURCE_INVALID = 0x7fffffff
} AclsanCallbackIdResource;

typedef enum AclsanCallbackIdSynchronize {
    ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END = 1, // 需要用到
    ACLSAN_CBID_SYNCHRONIZE_DEVICE_SYNC_END = 2,
    ACLSAN_CBID_SYNCHRONIZE_INVALID = 0x7fffffff
} AclsanCallbackIdSynchronize;

typedef enum AclsanCallbackIdDeviceInstruction {
    ACLSAN_CBID_DEVICE_MEMORY_ACCESS = 1, // 需要用到
    ACLSAN_CBID_DEVICE_SYNC = 2,          // 需要用到
    ACLSAN_CBID_DEVICE_STATE = 3,
    ACLSAN_CBID_DEVICE_CONTROL = 4,
    ACLSAN_CBID_DEVICE_ERROR = 5,
    ACLSAN_CBID_DEVICE_INSTRUCTION_INVALID = 0x7fffffff
} AclsanCallbackIdDeviceInstruction;

typedef enum AclsanCallbackIdDeviceBarrier {
    ACLSAN_CBID_DEVICE_PIPE_BARRIER = 1,
    ACLSAN_CBID_DEVICE_BARRIER_INVALID = 0x7fffffff
} AclsanCallbackIdDeviceBarrier;

typedef enum AclsanCallbackIdLaunch {
    ACLSAN_CBID_LAUNCH_KERNEL = 1,
    ACLSAN_CBID_LAUNCH_INVALID = 0x7fffffff
} AclsanCallbackIdLaunch;

// 通用 callback ABI 类型；具体含义由 AclsanCallbackDomain 和 ID 共同决定。
typedef uint32_t AclsanCallbackId;

typedef struct AclsanSubscriber* AclsanSubscriberHandle;
typedef void (*AclsanCallbackFunc)(
    void* userdata, AclsanCallbackDomain domain, AclsanCallbackId id, const void* cbdata);

// 同时只能有1个subscriber
ACLSAN_EXPORT AclsanStatus
aclsanSubscribe(AclsanSubscriberHandle* subscriber, AclsanCallbackFunc callback, void* userdata);
ACLSAN_EXPORT AclsanStatus aclsanUnsubscribe(AclsanSubscriberHandle subscriber);

// 以下相关函数不能被多个线程并发调用
ACLSAN_EXPORT AclsanStatus aclsanEnableCallback(
    uint32_t enable, AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, AclsanCallbackId cbid);
ACLSAN_EXPORT AclsanStatus
aclsanEnableDomain(AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, uint32_t enable);
ACLSAN_EXPORT AclsanStatus aclsanGetCallbackState(
    AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, AclsanCallbackId cbid, uint32_t* enabled);

// TODO: 需要测试多binary的时候怎么处理
// 查询当前活动 Device binary 中 pc 对应的静态 DWARF inline 调用栈。
// result 由调用方分配；结构体较大，建议在堆上分配或复用。
// binary unload 后重新 load 会获得新的 binaryId。
// 返回 ACLSAN_STATUS_ERROR_MAX_LIMIT_REACHED 时，result 中保留截断后的有效帧并设置 TRUNCATED 标志。
ACLSAN_EXPORT AclsanStatus aclsanGetDeviceCallStack(uint64_t pc, AclsanDeviceCallStack* result);
// TODO: 多.o场景的接口后面再加一个
#ifdef __cplusplus
}
#endif

#endif // NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_API_H
