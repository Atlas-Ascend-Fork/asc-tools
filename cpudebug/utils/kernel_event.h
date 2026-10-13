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
 * \file kernel_event.h
 * \brief
 */
#ifndef ASCENDC_KERNEL_EVENT_IMPL_H
#define ASCENDC_KERNEL_EVENT_IMPL_H

#include "kernel_macros.h"
#include "kernel_log.h"
#if defined(ASCENDC_CPU_DEBUG) && ASCENDC_CPU_DEBUG == 1
#include <cstdint>
#include "stub_def.h"
#include "stub_fun.h"
#endif

namespace AscendC {
enum class TPosition : uint8_t {
    GM,
    A1,
    A2,
    B1,
    B2,
    C1,
    C2,
    CO1,
    CO2,
    VECIN,
    VECOUT,
    VECCALC,
    LCM = VECCALC,
    SPM,
    SHM = SPM,
    TSCM,
    C2PIPE2GM,
    C2PIPE2LOCAL,
    MAX,
};

using QuePosition = TPosition;
enum class Hardware : uint8_t { GM, UB, L1, L0A, L0B, L0C, BIAS, FIXBUF, MAX };

enum class HardEvent : uint8_t {
    // src_dst
    MTE2_MTE1,
    MTE1_MTE2,
    MTE1_M,
    M_MTE1,
    MTE2_V,
    V_MTE2,
    MTE3_V,
    V_MTE3,
    M_V,
    V_M,
    V_V,
    MTE3_MTE1,
    MTE1_MTE3,
    MTE1_V,
    MTE2_M,
    M_MTE2,
    V_MTE1,
    M_FIX,
    FIX_M,
    MTE3_MTE2,
    MTE2_MTE3,
    S_V,
    V_S,
    S_MTE2,
    MTE2_S,
    S_MTE3,
    MTE3_S,
    MTE2_FIX,
    FIX_MTE2,
    FIX_S,
    M_S,
    FIX_MTE3,
    MTE1_FIX,
    FIX_MTE1,
    FIX_FIX,
#if defined(__NPU_ARCH__) && \
    ((__NPU_ARCH__ == 2103) || (__NPU_ARCH__ == 3003) || (__NPU_ARCH__ == 3103) || (__NPU_ARCH__ == 3113))
    M_MTE3,
    MTE3_M,
#endif
    MAX,
};

enum class HardEventAic : uint8_t {
    // src_dst
    MTE2_MTE1,
    MTE1_MTE2,
    MTE1_M,
    M_MTE1,
    MTE3_MTE1,
    MTE1_MTE3,
    MTE2_M,
    M_MTE2,
    M_FIX,
    FIX_M,
    MTE3_MTE2,
    MTE2_MTE3,
    S_MTE2,
    MTE2_S,
    S_MTE3,
    MTE3_S,
    MTE2_FIX,
    FIX_MTE2,
    FIX_S,
    M_S,
    FIX_MTE3,
    MTE1_FIX,
    FIX_MTE1,
    FIX_FIX,
#if defined(__NPU_ARCH__) && \
    ((__NPU_ARCH__ == 2103) || (__NPU_ARCH__ == 3003) || (__NPU_ARCH__ == 3103) || (__NPU_ARCH__ == 3113))
    M_MTE3,
    MTE3_M,
#endif
    MAX,
};

enum class HardEventAiv : uint8_t {
    // src_dst
    MTE2_V,
    V_MTE2,
    MTE3_V,
    V_MTE3,
    V_V,
    MTE3_MTE2,
    MTE2_MTE3,
    S_V,
    V_S,
    S_MTE2,
    MTE2_S,
    S_MTE3,
    MTE3_S,
    MAX,
};

enum class MemoryT : uint8_t { L1 = 0, L0A, L0B, L0C, UB, BIAS };

enum class MemDsbT : uint8_t { ALL = 0, DDR, UB, SEQ };

#if defined(__NPU_ARCH__) && (__NPU_ARCH__ != 1001) && (__NPU_ARCH__ != 2002)
constexpr int32_t PIPE_NUM = 7;
constexpr pipe_t SUPPORTED_PIPE[PIPE_NUM] = {PIPE_S, PIPE_V, PIPE_M, PIPE_MTE1, PIPE_MTE2, PIPE_MTE3, PIPE_FIX};
#else
constexpr int32_t PIPE_NUM = 6;
constexpr pipe_t SUPPORTED_PIPE[PIPE_NUM] = {PIPE_S, PIPE_V, PIPE_M, PIPE_MTE1, PIPE_MTE2, PIPE_MTE3};
#endif

#if defined(__NPU_ARCH__)
template <pipe_t pipe>
__aicore__ inline constexpr bool IsSplitVectorPipe()
{
    return pipe == PIPE_S || pipe == PIPE_V || pipe == PIPE_MTE2 || pipe == PIPE_MTE3;
}

template <pipe_t pipe>
__aicore__ inline constexpr bool IsSplitCubePipe()
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 1001 || __NPU_ARCH__ == 2002)
    return pipe == PIPE_S || pipe == PIPE_MTE1 || pipe == PIPE_MTE2 || pipe == PIPE_MTE3 || pipe == PIPE_M;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    return pipe == PIPE_S || pipe == PIPE_MTE1 || pipe == PIPE_MTE2 || pipe == PIPE_FIX || pipe == PIPE_M;
#else
    return pipe == PIPE_S || pipe == PIPE_MTE1 || pipe == PIPE_MTE2 || pipe == PIPE_MTE3 || pipe == PIPE_FIX ||
           pipe == PIPE_M;
#endif
}

template <pipe_t pipe>
__aicore__ inline void PipeBarrierInternal()
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    if constexpr (IsSplitVectorPipe<pipe>() || pipe == PIPE_ALL) {
        if ASCEND_IS_AIV {
            pipe_barrier(pipe);
        }
    }
    if constexpr (IsSplitCubePipe<pipe>() || pipe == PIPE_ALL) {
        if ASCEND_IS_AIC {
            pipe_barrier(pipe);
        }
    }
#else
    pipe_barrier(pipe);
#endif
}

template <pipe_t srcPipe, pipe_t dstPipe>
__aicore__ inline void SetFlagInternal(event_t evt)
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    if constexpr (IsSplitVectorPipe<srcPipe>() && IsSplitVectorPipe<dstPipe>()) {
        if ASCEND_IS_AIV {
            set_flag(srcPipe, dstPipe, evt);
        }
    }
    if constexpr (IsSplitCubePipe<srcPipe>() && IsSplitCubePipe<dstPipe>()) {
        if ASCEND_IS_AIC {
            set_flag(srcPipe, dstPipe, evt);
        }
    }
#else
    set_flag(srcPipe, dstPipe, evt);
#endif
}

template <pipe_t srcPipe, pipe_t dstPipe>
__aicore__ inline void WaitFlagInternal(event_t evt)
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    if constexpr (IsSplitVectorPipe<srcPipe>() && IsSplitVectorPipe<dstPipe>()) {
        if ASCEND_IS_AIV {
            wait_flag(srcPipe, dstPipe, evt);
        }
    }
    if constexpr (IsSplitCubePipe<srcPipe>() && IsSplitCubePipe<dstPipe>()) {
        if ASCEND_IS_AIC {
            wait_flag(srcPipe, dstPipe, evt);
        }
    }
#else
    wait_flag(srcPipe, dstPipe, evt);
#endif
    (void)evt;
}
#endif

__aicore__ constexpr Hardware GetPhyType(TPosition pos)
{
    ASSERT(pos != TPosition::MAX);
    Hardware hard = Hardware::UB;
    if (pos == TPosition::GM) {
        hard = Hardware::GM;
    } else if (pos == TPosition::A1) {
        hard = Hardware::L1;
    } else if (pos == TPosition::A2) {
        hard = Hardware::L0A;
    } else if (pos == TPosition::B1) {
        hard = Hardware::L1;
    } else if (pos == TPosition::B2) {
        hard = Hardware::L0B;
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 1001) || (__NPU_ARCH__ == 2002))
    } else if (pos == TPosition::C1) {
        hard = Hardware::UB;
    } else if (pos == TPosition::C2) {
        hard = Hardware::L0C;
    } else if (pos == TPosition::CO2) {
        hard = Hardware::UB;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 2103)
    } else if (pos == TPosition::C1) {
        hard = Hardware::L1;
    } else if (pos == TPosition::C2) {
        hard = Hardware::BIAS;
    } else if (pos == TPosition::CO2) {
        hard = Hardware::L1;
    } else if (pos == TPosition::C2PIPE2GM) {
        hard = Hardware::FIXBUF;
#elif (__NPU_ARCH__ == 2201)
    } else if (pos == TPosition::C1) {
        hard = Hardware::L1;
    } else if (pos == TPosition::C2) {
        hard = Hardware::BIAS;
    } else if (pos == TPosition::CO2) {
        hard = Hardware::GM;
    } else if (pos == TPosition::C2PIPE2GM) {
        hard = Hardware::FIXBUF;
#elif (__NPU_ARCH__ == 3002)
    } else if (pos == TPosition::C1) {
        hard = Hardware::L1;
    } else if (pos == TPosition::C2) {
        hard = Hardware::BIAS;
    } else if (pos == TPosition::C2PIPE2GM) {
        hard = Hardware::FIXBUF;
#elif (__NPU_ARCH__ == 3102)
    } else if (pos == TPosition::C1) {
        hard = Hardware::L1;
    } else if (pos == TPosition::C2) {
        hard = Hardware::BIAS;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3103)
    } else if (pos == TPosition::C1) {
        hard = Hardware::L1;
    } else if (pos == TPosition::C2) {
        hard = Hardware::BIAS;
    } else if (pos == TPosition::CO2) {
        hard = Hardware::GM;
    } else if (pos == TPosition::C2PIPE2GM) {
        hard = Hardware::FIXBUF;
#elif (__NPU_ARCH__ == 5102)
    } else if (pos == TPosition::C1) {
        hard = Hardware::L1;
    } else if (pos == TPosition::C2) {
        hard = Hardware::BIAS;
    } else if (pos == TPosition::CO2) {
        hard = Hardware::GM;
#elif defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 3113))
    } else if (pos == TPosition::C1) {
        hard = Hardware::L1;
    } else if (pos == TPosition::C2) {
        hard = Hardware::BIAS;
    } else if (pos == TPosition::CO2) {
        hard = Hardware::GM;
    } else if (pos == TPosition::C2PIPE2GM) {
        hard = Hardware::FIXBUF;
#endif
    } else if (pos == TPosition::CO1) {
        hard = Hardware::L0C;
    } else if (pos == TPosition::SHM) {
        hard = Hardware::L1;
    } else if (pos == TPosition::TSCM) {
        hard = Hardware::L1;
    }
    return hard;
}

__aicore__ constexpr TPosition GetPosition(TPosition srcPos, TPosition dstPos)
{
    // unsupported data stream
    ASSERT(!((srcPos == TPosition::CO2) && (dstPos == TPosition::SHM)));
    ASSERT(!((srcPos == TPosition::VECOUT) && (dstPos == TPosition::SHM)));
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 1001) || (__NPU_ARCH__ == 2002))
    if (dstPos == TPosition::GM || ((dstPos == TPosition::CO2) && (srcPos == TPosition::CO1))) {
        return srcPos;
    }
#elif defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 2201) || (__NPU_ARCH__ == 3002) || (__NPU_ARCH__ == 3102) || \
                                (__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102) || (__NPU_ARCH__ == 2103) || \
                                (__NPU_ARCH__ == 3003) || (__NPU_ARCH__ == 3103) || (__NPU_ARCH__ == 3113))
    if ((dstPos == TPosition::GM) || (dstPos == TPosition::CO2)) {
        return srcPos;
    }
#endif
    return dstPos;
}

#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
#ifdef ASCENDC_CPU_DEBUG
class BufIdTracker {
public:
    struct BufState {
        pipe_t pipe;
        bool mode;
        bool released;
    };

    static BufIdTracker& GetInstance()
    {
        static BufIdTracker tracker;
        return tracker;
    }

    bool AddBufIdEvent(uint8_t bufId, pipe_t pipe, bool mode, bool released);
    bool GetState();
    void Reset();

private:
    BufIdTracker() {}
    bool UpdateState(uint8_t bufId, pipe_t pipe, bool mode, bool released);

    std::map<uint8_t, BufState> bufIdMapAiv_;
    std::map<uint8_t, BufState> bufIdMapAic_;

    bool aivState_ = true;
    bool aicState_ = true;
};
#endif

using TBufId = uint8_t;
constexpr TBufId MAX_TBUFID = (static_cast<TBufId>(31));
#endif

} // namespace AscendC

#endif // ASCENDC_KERNEL_EVENT_IMPL_H
