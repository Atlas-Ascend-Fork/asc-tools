/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef INJECTION_STUBS_PROF_API_INCLUDE_PROFILING_PROF_COMMON_H_
#define INJECTION_STUBS_PROF_API_INCLUDE_PROFILING_PROF_COMMON_H_

#include <cstddef>
#include <cstdint>

#define MSPROF_MAX_DEV_NUM 64
#define MAX_DUMP_PATH_LEN 1024
#define MAX_SAMPLE_CONFIG_LEN 4096
#define RAW_DATA_MAXSIZE 256

#define PROF_AICORE_METRICS_MASK 0x00000004ULL
#define PROF_TASK_TIME_L1_MASK 0x00000002ULL
#define PROF_AICPU_TRACE_MASK 0x00000008ULL
#define PROF_OP_TIMESTAMP_MASK 0x100000000ULL
#define PROF_TASK_TIME_MASK 0x00000800ULL
#define PROF_INSTR_MASK 0x00800000ULL

#define COMPUTE_AICORE_METRICS_NUM 10
#define MSPROF_INVALID_AICORE_METRIC UINT32_MAX
#define MSPROF_CONFIG_ATTR_MAX_NUM 16

enum MsprofConfigAttrId {
    PROF_CONFIG_ATTR_AICORE_METRICS = 0,
    PROF_CONFIG_ATTR_INSTR = 1,
    PROF_CONFIG_ATTR_TASK_BLOCK = 2
};

enum MsprofComputeInstrMode { PROF_COMPUTE_BIU_PERF = 1, PROF_COMPUTE_PC_SAMPLING = 2 };

enum MsprofComputeBlockMode { PROF_COMPUTE_ALL_BLOCK = 1, PROF_COMPUTE_BLOCK_SHRINK = 2 };

union MsprofConfigAttrValue {
    uint32_t aicoreMetrics[COMPUTE_AICORE_METRICS_NUM];
    uint32_t instrMode;
    uint32_t taskBlockMode;
};

struct MsprofConfigAttr {
    uint32_t id;
    union MsprofConfigAttrValue value;
};

struct MsprofConfigInfo {
    size_t numAttrs;
    const struct MsprofConfigAttr* attrs;
};

struct MsprofConfig {
    uint64_t profSwitch;
    uint64_t profSwitchHi;
    uint32_t devNums;
    uint32_t devIdList[MSPROF_MAX_DEV_NUM + 1];
    uint32_t modelId;
    uint32_t type;
    uint32_t cacheFlag;
    uint32_t storageLimit;
    uint32_t metrics;
    uintptr_t fd;
    char dumpPath[MAX_DUMP_PATH_LEN];
    char sampleConfig[MAX_SAMPLE_CONFIG_LEN];
    struct MsprofConfigInfo configInfo;
};

enum RawDataType {
    API_DATA_TYPE = 0,
    EVENT_DATA_TYPE = 1,
    RUNTIMETRACK_DATA_TYPE = 3,
    PMU_DATA_TYPE = 4,
    DEFAULT_DATA_TYPE = 5,
    LOG_DATA_TYPE = 7,
    BIU_PERF_DATA_TYPE = 8,
    PC_SAMPLING_DATA_TYPE = 9,
    BIU_PERF_AIV0_DATA_TYPE = 10,
    BIU_PERF_AIV1_DATA_TYPE = 11,
    AICPU_DATA_TYPE = 12,
    CCU_INSTR_DATA_TYPE = 13,
    CCU_STAT_DATA_TYPE = 14,
    API_EVENT_DATA_TYPE = 15,
    COMPACT_DATA_TYPE = 16,
    ADDITIONAL_DATA_TYPE = 17
};

typedef struct {
    bool isLastChunk;
    size_t offset;
    int32_t chunkModule;
    int32_t deviceId;
    enum RawDataType type;
    size_t chunkSize;
    char chunk[RAW_DATA_MAXSIZE];
} MsprofRawData;

typedef int32_t (*MsprofRawDataCallback)(MsprofRawData* rawData);

#endif // INJECTION_STUBS_PROF_API_INCLUDE_PROFILING_PROF_COMMON_H_
