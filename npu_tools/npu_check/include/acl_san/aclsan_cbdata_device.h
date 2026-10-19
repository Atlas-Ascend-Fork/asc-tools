/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_DEVICE_H
#define NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_DEVICE_H

#include "acl_san/aclsan_cbdata_common.h"

#define ACLSAN_RAW_ARG_MAX 6

// 映射cce侧的PIPE_X值
typedef enum AclsanDevicePipeline {
    ACLSAN_DEVICE_PIPE_SCALAR = 0,
    ACLSAN_DEVICE_PIPE_VECTOR = 1,
    ACLSAN_DEVICE_PIPE_MATRIX = 2,
    ACLSAN_DEVICE_PIPE_MTE1 = 3,
    ACLSAN_DEVICE_PIPE_MTE2 = 4,
    ACLSAN_DEVICE_PIPE_MTE3 = 5,
    ACLSAN_DEVICE_PIPE_ALL = 6, // TODO: 暂定留着
    ACLSAN_DEVICE_PIPE_FIXPIPE = 10,
    ACLSAN_DEVICE_PIPE_INVALID = 100,
} AclsanDevicePipeline;

typedef enum AclsanDeviceBlockType {
    ACLSAN_DEVICE_BLOCK_TYPE_AICORE = 0,
    ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR = 1,
    ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE = 2,
} AclsanDeviceBlockType;

// TODO: 目前暂时没地方用到，需要看看能不能删掉
// TODO: 后续看怎么分合理。
typedef enum AclsanDeviceSourceKind {
    ACLSAN_DEVICE_SOURCE_UNKNOWN = 0,
    ACLSAN_DEVICE_SOURCE_MTE2 = 1,
    ACLSAN_DEVICE_SOURCE_MTE3 = 2,
    ACLSAN_DEVICE_SOURCE_FIXPIPE = 3,
    ACLSAN_DEVICE_SOURCE_SET_WAIT_FLAG = 4,
    ACLSAN_DEVICE_SOURCE_GET_RLS_BUF = 5,
    ACLSAN_DEVICE_SOURCE_LD = 6,
    ACLSAN_DEVICE_SOURCE_ST = 7,
    ACLSAN_DEVICE_SOURCE_VECTOR = 8,
    ACLSAN_DEVICE_SOURCE_CUBE = 9,
    ACLSAN_DEVICE_SOURCE_SCALAR = 10
} AclsanDeviceSourceKind;

// device对应的物理位置
typedef enum AclsanDeviceMemorySpace {
    ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN = 0,
    ACLSAN_DEVICE_MEMORY_SPACE_GM = 1,
    ACLSAN_DEVICE_MEMORY_SPACE_UB = 2,
    ACLSAN_DEVICE_MEMORY_SPACE_L1 = 3,
    ACLSAN_DEVICE_MEMORY_SPACE_L0A = 4,
    ACLSAN_DEVICE_MEMORY_SPACE_L0B = 5,
    ACLSAN_DEVICE_MEMORY_SPACE_L0C = 6,
    ACLSAN_DEVICE_MEMORY_SPACE_BT = 7,
    ACLSAN_DEVICE_MEMORY_SPACE_FB = 8
} AclsanDeviceMemorySpace;

typedef enum AclsanDeviceMemoryAccessMode {
    ACLSAN_DEVICE_MEMORY_ACCESS_READ = 1,
    ACLSAN_DEVICE_MEMORY_ACCESS_WRITE = 2,
    ACLSAN_DEVICE_MEMORY_ACCESS_READ_WRITE = 3 // TODO: 后续看有没有atomic
} AclsanDeviceMemoryAccessMode;

// TODO: 先留着，不确定可靠性有没有必要
typedef enum AclsanDeviceEventFlags {
    ACLSAN_DEVICE_EVENT_FLAG_NONE = 0,
    ACLSAN_DEVICE_EVENT_FLAG_EXACT = 1u << 0u,
    ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED = 1u << 1u,
    ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED = 1u << 2u,
    ACLSAN_DEVICE_EVENT_FLAG_PREDICATED = 1u << 3u,
    ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR = 1u << 4u
} AclsanDeviceEventFlags;

typedef struct AclsanDeviceEventHeader {
    uint32_t version;
    uint32_t size;
    uint64_t launchId;
    uint64_t pc;
    uint32_t siteId;      // 插桩id
    uint32_t sourceKind;  // AclsanDeviceSourceKind
    uint64_t instrExecId; // TODO: 看下和serialNo之间的关系
    uint64_t serialNo;    //       例如：指令DataCopy中的第一(read) / 第二条 (write)
    uint32_t deviceId;
    uint32_t phyCoreId;
    uint32_t blockId;
    uint32_t blockType; // AclsanDeviceBlockType
    uint32_t pipeline;  // AclsanDevicePipeline
    uint32_t flags;     // AclsanDeviceEventFlags
} AclsanDeviceEventHeader;

// Delivered only through ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION.
typedef struct AclsanDeviceBarrierData {
    AclsanDeviceEventHeader header;
    uint32_t pipe; // Barrier target; header.pipeline is the issuing pipeline.
    uint32_t reserved;
} AclsanDeviceBarrierData;

// ==========================================
// ===========      MEM CHECK      ==========
// ==========================================
// 内存访问模板: 单点访问 / 连续范围 / stride跳跃 / 多维
typedef enum AclsanMemLayoutKind {
    ACLSAN_MEM_LAYOUT_SCALAR = 1,
    ACLSAN_MEM_LAYOUT_RANGE = 2,
    ACLSAN_MEM_LAYOUT_BLOCK_REPEAT = 3,
    ACLSAN_MEM_LAYOUT_ND_AFFINE = 4
} AclsanMemLayoutKind;

typedef struct AclsanMemScalarLayout {
    uint32_t bytes;
    uint32_t reserved;
} AclsanMemScalarLayout;

typedef struct AclsanMemRangeLayout {
    uint64_t bytes;
} AclsanMemRangeLayout;

typedef struct AclsanMemBlockRepeatLayout {
    uint32_t blockNum;
    uint32_t blockSize;
    int64_t blockStride; // 前一个block的头 到 下一个block的头，单位为1 Byte
    uint32_t repeatTimes;
    uint32_t reserved;
    int64_t repeatStride;
} AclsanMemBlockRepeatLayout;

typedef struct AclsanMemNdAffineLayout {
    uint32_t rank;
    uint32_t elementBytes;
    uint64_t dims[5];
    int64_t strides[5];
} AclsanMemNdAffineLayout;

typedef struct AclsanDeviceMemoryAccessData {
    AclsanDeviceEventHeader header;
    uint64_t address;
    uint32_t memorySpace; // AclsanDeviceMemorySpace
    uint32_t accessMode;  // AclsanDeviceMemoryAccessMode
    uint32_t accessIndex; // TODO: 待确认作用   确认下第一条和第二条是用的哪个 serialNo
    uint32_t accessCount; // TODO: 待确认作用
    uint32_t dataBits;    // TODO: 看能否去掉
    uint32_t alignSize;
    uint64_t vectorMask0;
    uint64_t vectorMask1;
    uint64_t predicateMask0;
    uint64_t predicateMask1;
    uint32_t layoutKind;
    uint32_t memoryFlags;
    union {
        AclsanMemScalarLayout scalar;
        AclsanMemRangeLayout range;
        AclsanMemBlockRepeatLayout blockRepeat;
        AclsanMemNdAffineLayout ndAffine;
    } layout;
    uint32_t instructionId;
    uint32_t unitFlag; // Former reserved slot: 0=disabled, 2=keep, 3=update L0C unit flag.
    uint64_t regDependencyMask0;
    uint64_t regDependencyMask1;
} AclsanDeviceMemoryAccessData;

typedef enum AclsanDeviceRegisterId {
    ACLSAN_DEVICE_REGISTER_VECTOR_MASK = 0,
    ACLSAN_DEVICE_REGISTER_MTE2_SOURCE = 1,
    ACLSAN_DEVICE_REGISTER_NDDMA_PAD_COUNT = 2,
    ACLSAN_DEVICE_REGISTER_NDDMA_LOOP0_STRIDE = 3,
    ACLSAN_DEVICE_REGISTER_NDDMA_LOOP1_STRIDE = 4,
    ACLSAN_DEVICE_REGISTER_NDDMA_LOOP2_STRIDE = 5,
    ACLSAN_DEVICE_REGISTER_NDDMA_LOOP3_STRIDE = 6,
    ACLSAN_DEVICE_REGISTER_NDDMA_LOOP4_STRIDE = 7,
    ACLSAN_DEVICE_REGISTER_MTE2_NZ = 8,
    ACLSAN_DEVICE_REGISTER_LOOP3 = 9,
    ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_UBUF_TO_GM = 10,
    ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_GM_TO_UBUF = 11,
    ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_GM_TO_CBUF = 12,
    ACLSAN_DEVICE_REGISTER_DMA_LOOP1_STRIDE_UBUF_TO_GM = 13,
    ACLSAN_DEVICE_REGISTER_DMA_LOOP2_STRIDE_UBUF_TO_GM = 14,
    ACLSAN_DEVICE_REGISTER_DMA_LOOP1_STRIDE_GM_TO_UBUF = 15,
    ACLSAN_DEVICE_REGISTER_DMA_LOOP2_STRIDE_GM_TO_UBUF = 16,
    ACLSAN_DEVICE_REGISTER_DMA_LOOP1_STRIDE_GM_TO_CBUF = 17,
    ACLSAN_DEVICE_REGISTER_DMA_LOOP2_STRIDE_GM_TO_CBUF = 18,
    ACLSAN_DEVICE_REGISTER_SET_PADDING = 19,
    ACLSAN_DEVICE_REGISTER_COUNT = 20
} AclsanDeviceRegisterId;

typedef struct AclsanDeviceRegisterStateData {
    AclsanDeviceEventHeader header;
    uint32_t regId; // AclsanDeviceRegisterId
    uint32_t reserved;
    uint64_t value;
} AclsanDeviceRegisterStateData;

// ==========================================
// ===========     SYNC CHECK      ==========
// ==========================================

// TODO: 源神想想synckind + action + scope如何保留信息
typedef enum AclsanDeviceSyncKind {
    ACLSAN_DEVICE_SYNC_KIND_UNKNOWN = 0,
    ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG = 1,
    ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF = 2
} AclsanDeviceSyncKind;

// TODO: 看SET / GET能否合并   OPEN / CLOSE ?
typedef enum AclsanDeviceSyncAction {
    ACLSAN_DEVICE_SYNC_ACTION_UNKNOWN = 0,
    ACLSAN_DEVICE_SYNC_ACTION_SET = 1, // set / wait flag
    ACLSAN_DEVICE_SYNC_ACTION_WAIT = 2,
    ACLSAN_DEVICE_SYNC_ACTION_GET = 3, // get / rls buf
    ACLSAN_DEVICE_SYNC_ACTION_RELEASE = 4
} AclsanDeviceSyncAction;

// TODO: 需要写出每个scope对应哪些指令
typedef enum AclsanDeviceSyncScope {
    ACLSAN_DEVICE_SYNC_SCOPE_UNKNOWN = 0,
    ACLSAN_DEVICE_SYNC_SCOPE_PIPE = 1, // 流水内 / 流水间有没有办法区分
    ACLSAN_DEVICE_SYNC_SCOPE_BLOCK = 2,
    ACLSAN_DEVICE_SYNC_SCOPE_CORE = 3,
    ACLSAN_DEVICE_SYNC_SCOPE_CLUSTER = 4,
    ACLSAN_DEVICE_SYNC_SCOPE_DEVICE = 5,
    ACLSAN_DEVICE_SYNC_SCOPE_SOC = 6
} AclsanDeviceSyncScope;

typedef struct AclsanDeviceSyncData {
    AclsanDeviceEventHeader header;
    uint32_t syncKind; // AclsanDeviceSyncKind
    uint32_t action;   // AclsanDeviceSyncAction
    uint32_t scope;    // AclsanDeviceSyncScope
    uint32_t srcPipe;
    uint32_t dstPipe;  // GET_BUF的pipe直接写dstPipe
    uint32_t mode;     // mode for GET_BUF / RLS_BUF
    uint64_t objectId; // event id for SET_FLAG, bufId for GET_BUF
    uint32_t reserved[2];
} AclsanDeviceSyncData;

#endif // NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_DEVICE_H
