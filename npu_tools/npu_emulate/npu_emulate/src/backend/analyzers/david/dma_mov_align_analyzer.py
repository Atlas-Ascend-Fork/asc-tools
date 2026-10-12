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

import sqlite3
from typing import Optional

from npu_emulate.src.common.extend_params import ExtendParams
from npu_emulate.src.backend.analyzers.base.dma_mov_analyzer_base import DMAInstrInfo
from npu_emulate.src.backend.analyzers.base.dma_mov_analyzer_simple import (
    DMAMovAnalyzerSimple,
    DMAMovSimpleParamsInfo,
)


class Ascend950DMAMovAlignAnalyzer(DMAMovAnalyzerSimple):
    def __init__(self) -> None:
        super().__init__()
        self.instr_params["MOV_SRC_TO_DST_ALIGNv2"] = {}

    @staticmethod
    def get_name() -> str:
        return "ascend950_dma_mov_align_analyzer"

    def process_instruction(self, instr: sqlite3.Row) -> Optional[DMAInstrInfo]:
        ep = ExtendParams.parse(instr["ExtendParams"])
        burst_len = ep.burst_len
        burst_number = ep.burst_number
        src_stride = ep.src_stride
        dst_stride = ep.dst_stride
        processed_bytes = ep.processed_bytes
        if any(v is None for v in (burst_len, burst_number, src_stride, dst_stride)):
            return None
        return DMAInstrInfo(
            DMAMovSimpleParamsInfo(burst_len, burst_number, src_stride, dst_stride),
            processed_bytes,
        )
