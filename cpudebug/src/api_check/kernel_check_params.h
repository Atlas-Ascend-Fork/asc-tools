/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file kernel_check_params.h
 * \brief
 */

#ifndef ASCENDC_CHECK_PARAMS_H
#define ASCENDC_CHECK_PARAMS_H
#include <map>
#include "dlog_pub.h"
#include "kernel_utils.h"
#ifdef __DAV_M200__
#include "ascend610_ini.h"
#elif defined __DAV_C100__
#include "ascend910_ini.h"
#elif defined __DAV_C220__
#include "ascend910B1_ini.h"
#elif defined __DAV_M300__ || (defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3003))
#include "ascend310B1_ini.h"
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3102 || __NPU_ARCH__ == 3103 || __NPU_ARCH__ == 3113)
#include "ascend610Lite_ini.h"
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
#include "ascend950pr_9599_ini.h"
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 5102)
#include "mc62cm12aa_ini.h"
#endif

namespace AscendC {
namespace check {
#define ASCENDC_CHECK(x)  \
    do {                  \
        if (!(x)) {       \
            return false; \
        }                 \
    } while (0)

#define ASCENDC_CHECK_AND_LOG(cond, behavior) \
    do {                                      \
        if (!(cond)) {                        \
            behavior;                         \
            return false;                     \
        }                                     \
    } while (0)

#define ASCENDC_MODULE_NAME static_cast<int32_t>(ASCENDCKERNEL)

#define CHECK_LOG_DEBUG(format, ...)                                 \
    do {                                                             \
        dlog_debug(ASCENDC_MODULE_NAME, format "\n", ##__VA_ARGS__); \
    } while (0)

#define CHECK_LOG_INFO(format, ...)                                 \
    do {                                                            \
        dlog_info(ASCENDC_MODULE_NAME, format "\n", ##__VA_ARGS__); \
    } while (0)

#define CHECK_LOG_WARNING(format, ...)                              \
    do {                                                            \
        dlog_warn(ASCENDC_MODULE_NAME, format "\n", ##__VA_ARGS__); \
    } while (0)

#define CHECK_LOG_ERROR(format, ...)                                 \
    do {                                                             \
        printf("[ERROR]" format "\n", ##__VA_ARGS__);                \
        dlog_error(ASCENDC_MODULE_NAME, format "\n", ##__VA_ARGS__); \
    } while (0)

enum class HardWareIndex { GM = 0, UB, L1, L0A, L0B, L0C, BIAS, FIXBUF, MAX };

class GlobalParams {
public:
    static GlobalParams& Instance()
    {
        static GlobalParams instance;
        return instance;
    }

    const std::map<uint8_t, std::string> hardwareNameMap{
        {static_cast<uint8_t>(HardWareIndex::GM), "GM"},     {static_cast<uint8_t>(HardWareIndex::UB), "UB"},
        {static_cast<uint8_t>(HardWareIndex::L1), "L1"},     {static_cast<uint8_t>(HardWareIndex::L0A), "L0A"},
        {static_cast<uint8_t>(HardWareIndex::L0B), "L0B"},   {static_cast<uint8_t>(HardWareIndex::L0C), "L0C"},
        {static_cast<uint8_t>(HardWareIndex::BIAS), "BIAS"}, {static_cast<uint8_t>(HardWareIndex::FIXBUF), "FIXBUF"},
    };

    const std::map<uint8_t, uint64_t> bufferSizeMap{
        {static_cast<uint8_t>(HardWareIndex::UB), static_cast<uint64_t>(PlatFormParams::UB_SIZE)},
        {static_cast<uint8_t>(HardWareIndex::L1), static_cast<uint64_t>(PlatFormParams::L1_SIZE)},
        {static_cast<uint8_t>(HardWareIndex::L0A), static_cast<uint64_t>(PlatFormParams::L0A_SIZE)},
        {static_cast<uint8_t>(HardWareIndex::L0B), static_cast<uint64_t>(PlatFormParams::L0B_SIZE)},
        {static_cast<uint8_t>(HardWareIndex::L0C), static_cast<uint64_t>(PlatFormParams::L0C_SIZE)},
    };

private:
    GlobalParams() = default;
    ~GlobalParams() = default;
};

enum class TypeBitLen {
    K_B1_BITS = 1,
    K_B4_BITS = 4,
    K_B8_BITS = 8,
    K_B16_BITS = 16,
    K_B24_BITS = 24,
    K_B32_BITS = 32,
    K_B48_BITS = 48,
    K_B54_BITS = 54,
    K_B64_BITS = 64,
};

enum class TypeByteLen {
    K_B1_BYTE = 1,
    K_B2_BYTE = 2,
    K_B4_BYTE = 4,
    K_B8_BYTE = 8,
};

enum class CommonParams {
    MASK_MAX_ELE_LEN = 64,
    MASK_HIGH_IDX = 0,
    MASK_LOW_IDX = 1,
};

enum class ReduceCheckExtParams {
    VREDUCE_PER_REP_OUTPUT = 2,
    VREDUCE_CALL_INDEX_COUNT = 2,
    VREDUCE_BLK_DST_COUNT_MIN = 8,
};

enum class MaddCheckExtParams {
    MMAD_RANGE_MAX = 4095,
};
} // namespace check
} // namespace AscendC
#endif
