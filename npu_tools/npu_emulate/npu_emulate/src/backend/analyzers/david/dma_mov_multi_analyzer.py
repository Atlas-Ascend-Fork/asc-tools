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
from dataclasses import dataclass

from npu_emulate.src.common.extend_params import ExtendParams
from npu_emulate.src.backend.analyzers.base.dma_mov_analyzer_base import (
    DMAMovAnalyzerBase,
    DMAInstrInfo,
    DMAParamsInfo,
)
from npu_emulate.src.backend.database import database_handler


@dataclass
class DMAMovMultiParamsInfo(DMAParamsInfo):
    n: int
    d: int


class Ascend950DMAMovMultiAnalyzer(DMAMovAnalyzerBase):
    def __init__(self) -> None:
        super().__init__()
        self.instr_params["MOV_OUT_TO_L1_MULTI_ND2NZ"] = {}
        self.instr_params["MOV_OUT_TO_L1_MULTI_DN2NZ"] = {}

    @staticmethod
    def get_name() -> str:
        return "ascend950_dma_mov_multi_analyzer"

    def process_instruction(self, instr: sqlite3.Row) -> Optional[DMAInstrInfo]:
        ep = ExtendParams.parse(instr["ExtendParams"])
        n = ep.n
        d = ep.d
        processed_bytes = ep.processed_bytes
        if n is None or d is None:
            return None
        return DMAInstrInfo(
            DMAMovMultiParamsInfo(n, d),
            processed_bytes,
        )

    @classmethod
    def save_dma_params_info(cls, instr_id: int, params_info: DMAParamsInfo) -> None:
        assert isinstance(params_info, DMAMovMultiParamsInfo)
        database_handler.execute(
            """
            INSERT INTO DmaMovMultiParams
            (ExecInstrId,
             N,
             D)
            VALUES
            (?, ?, ?)
        """,
            (
                instr_id,
                params_info.n,
                params_info.d,
            ),
        )
