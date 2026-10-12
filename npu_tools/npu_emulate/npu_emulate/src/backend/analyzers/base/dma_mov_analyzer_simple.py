# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

from dataclasses import dataclass
from typing import Optional
from npu_emulate.src.backend.analyzers.base.dma_mov_analyzer_base import (
    DMAMovAnalyzerBase,
    DMAParamsInfo,
)
from npu_emulate.src.backend.database import database_handler


@dataclass
class DMAMovSimpleParamsInfo(DMAParamsInfo):
    burst_len: int
    burst_number: int
    src_stride: int
    dst_stride: int
    # is specific to some instructions (for example, MOV_L1_TO_UB)
    sub_block_id: Optional[int] = None


class DMAMovAnalyzerSimple(DMAMovAnalyzerBase):
    @classmethod
    def save_dma_params_info(cls, instr_id: int, params_info: DMAParamsInfo) -> None:
        assert isinstance(params_info, DMAMovSimpleParamsInfo)
        database_handler.execute(
            """
            INSERT INTO DmaMovSimpleParams
            (ExecInstrId,
             BurstLength,
             BurstNumber,
             SrcStride,
             DstStride,
             SubBlockId
            )
            VALUES
            (:exec_instr_id,
             :burst_len,
             :burst_number,
             :src_stride,
             :dst_stride,
             :sub_block_id
            )
        """,
            {
                "exec_instr_id": instr_id,
                "burst_len": params_info.burst_len,
                "burst_number": params_info.burst_number,
                "src_stride": params_info.src_stride,
                "dst_stride": params_info.dst_stride,
                "sub_block_id": params_info.sub_block_id,
            },
        )
