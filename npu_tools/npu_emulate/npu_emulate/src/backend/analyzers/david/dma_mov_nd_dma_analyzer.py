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
from dataclasses import dataclass
from typing import Optional

from npu_emulate.src.common.extend_params import ExtendParams
from npu_emulate.src.backend.analyzers.base.dma_mov_analyzer_base import (
    DMAMovAnalyzerBase,
    DMAInstrInfo,
    DMAParamsInfo,
)
from npu_emulate.src.backend.database import database_handler


@dataclass
class DMANdDmaParamsInfo(DMAParamsInfo):
    loop0_size: int
    loop1_size: int
    loop2_size: int
    loop3_size: int
    loop4_size: int
    loop0_lp_size: int
    loop0_rp_size: int


class Ascend950DMAMovNdDmaAnalyzer(DMAMovAnalyzerBase):
    def __init__(self) -> None:
        super().__init__()
        self.instr_params["ND_DMA"] = {}

    @staticmethod
    def get_name() -> str:
        return "ascend950_dma_mov_nd_dma_analyzer"

    def process_instruction(self, instr: sqlite3.Row) -> Optional[DMAInstrInfo]:
        ep = ExtendParams.parse(instr["ExtendParams"])
        loop0_size = ep.loop0_size
        loop1_size = ep.loop1_size
        loop2_size = ep.loop2_size
        loop3_size = ep.loop3_size
        loop4_size = ep.loop4_size
        loop0_lp_size = ep.loop0_lp_size
        loop0_rp_size = ep.loop0_rp_size
        processed_bytes = ep.processed_bytes
        if any(
            v is None
            for v in (
                loop0_size,
                loop1_size,
                loop2_size,
                loop3_size,
                loop4_size,
                loop0_lp_size,
                loop0_rp_size,
            )
        ):
            return None
        return DMAInstrInfo(
            DMANdDmaParamsInfo(
                loop0_size,
                loop1_size,
                loop2_size,
                loop3_size,
                loop4_size,
                loop0_lp_size,
                loop0_rp_size,
            ),
            processed_bytes,
        )

    @classmethod
    def save_dma_params_info(cls, instr_id: int, params_info: DMAParamsInfo) -> None:
        assert isinstance(params_info, DMANdDmaParamsInfo)
        database_handler.execute(
            """
            INSERT INTO DmaMovNdDmaParams
            (ExecInstrId,
             Loop0Size,
             Loop1Size,
             Loop2Size,
             Loop3Size,
             Loop4Size,
             Loop0LpSize,
             Loop0RpSize)
            VALUES
            (?, ?, ?, ?, ?, ?, ?, ?)
        """,
            (
                instr_id,
                params_info.loop0_size,
                params_info.loop1_size,
                params_info.loop2_size,
                params_info.loop3_size,
                params_info.loop4_size,
                params_info.loop0_lp_size,
                params_info.loop0_rp_size,
            ),
        )
