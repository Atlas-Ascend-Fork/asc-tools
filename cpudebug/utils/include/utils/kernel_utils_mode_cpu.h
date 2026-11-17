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
 * \file kernel_utils_mode_cpu.h
 * \brief
 */
#ifndef ASCENDC_MODULE_UTILS_MODE_CPU_H
#define ASCENDC_MODULE_UTILS_MODE_CPU_H
#include "utils/kernel_utils_constants.h"
#if defined(ASCENDC_CPU_DEBUG) && ASCENDC_CPU_DEBUG == 1
#include "utils/kernel_utils_const_definer.h"
#endif

namespace AscendC {
class MaskSetter {
public:
    static MaskSetter& Instance()
    {
        static MaskSetter instance;
        return instance;
    };

    void SetMask(bool setMask) { isSetMask = setMask; }

    bool GetMask() const { return isSetMask; }

private:
    MaskSetter() {};
    ~MaskSetter() {};
    bool isSetMask = true;
};

class Int4Setter {
public:
    static Int4Setter& Instance()
    {
        static Int4Setter instance;
        return instance;
    };

    void SetInt4() { isInt4 = true; }

    void SetDstInt4() { isDstInt4 = true; }

    void SetSrcInt4() { isSrcInt4 = true; }

    void ResetInt4() { isInt4 = false; }

    void ResetDstSrcInt4()
    {
        isDstInt4 = false;
        isSrcInt4 = false;
    }

    bool GetInt4() const { return isInt4; }

    bool GetDstInt4() const { return isDstInt4; }

    bool GetSrcInt4() const { return isSrcInt4; }

private:
    Int4Setter() {};
    ~Int4Setter() {};

    bool isInt4 = false;
    bool isDstInt4 = false;
    bool isSrcInt4 = false;
};

#if defined(ASCENDC_CPU_DEBUG) && ASCENDC_CPU_DEBUG == 1
inline std::string GetPositionDisplay(Hardware hardPos, const std::string& logicName)
{
    if (hardPos == Hardware::GM && logicName == "GM") {
        return "GM";
    }

    const char* hardName = "UNKNOWN";
    switch (hardPos) {
        case Hardware::GM:
            hardName = "GM";
            break;
        case Hardware::UB:
            hardName = "UB";
            break;
        case Hardware::L1:
            hardName = "L1 Buffer";
            break;
        case Hardware::L0A:
            hardName = "L0A Buffer";
            break;
        case Hardware::L0B:
            hardName = "L0B Buffer";
            break;
        case Hardware::L0C:
            hardName = "L0C Buffer";
            break;
        case Hardware::BIAS:
            hardName = "BiasTable Buffer";
            break;
        case Hardware::FIXBUF:
            hardName = "Fixpipe Buffer";
            break;
#if defined(__NPU_ARCH__) && \
    (__NPU_ARCH__ == 5101 || __NPU_ARCH__ == 5161 || __NPU_ARCH__ == 5165 || __NPU_ARCH__ == 5163)
        case Hardware::PT:
            hardName = "PaddingTable Buffer";
            break;
#endif
        default:
            break;
    }
    return std::string(hardName) + "(" + logicName + ")";
}

inline std::string GetPositionDisplay(TPosition pos)
{
    const auto& definer = ConstDefiner::Instance();
    const auto hardPos = definer.positionHardMap.find(pos);
    const Hardware hardware = hardPos == definer.positionHardMap.end() ? GetPhyType(pos) : hardPos->second;
    return GetPositionDisplay(hardware, definer.logicNameMap.at(static_cast<uint8_t>(pos)));
}
#endif

} // namespace AscendC
#endif // ASCENDC_MODULE_UTILS_MODE_H
