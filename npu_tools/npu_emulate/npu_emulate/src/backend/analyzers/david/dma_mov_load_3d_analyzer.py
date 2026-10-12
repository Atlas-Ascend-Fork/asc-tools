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
class DMAMovLoad3dParamsInfo(DMAParamsInfo):
    m_step: int
    k_step: int
    dst: str


class Ascend950DMAMovLoad3DAnalyzer(DMAMovAnalyzerBase):
    def __init__(self) -> None:
        super().__init__()
        self.instr_params["LOAD_L1_TO_DST_3DV2"] = {}

    @staticmethod
    def get_name() -> str:
        return "ascend950_dma_mov_load_3d_analyzer"

    def process_instruction(self, instr: sqlite3.Row) -> Optional[DMAInstrInfo]:
        ep = ExtendParams.parse(instr["ExtendParams"])
        m_step = ep.m_step
        k_step = ep.k_step
        processed_bytes = ep.processed_bytes
        dst = ep.dst_mem
        if any(v is None for v in (m_step, k_step, dst)):
            return None
        return DMAInstrInfo(
            DMAMovLoad3dParamsInfo(m_step, k_step, dst),
            processed_bytes,
        )

    @classmethod
    def save_dma_params_info(cls, instr_id: int, params_info: DMAParamsInfo) -> None:
        assert isinstance(params_info, DMAMovLoad3dParamsInfo)
        database_handler.execute(
            """
            INSERT INTO DmaMovLoad3dParams
            (ExecInstrId,
             MStep,
             KStep,
             Dst)
            VALUES
            (?, ?, ?, ?)
        """,
            (
                instr_id,
                params_info.m_step,
                params_info.k_step,
                params_info.dst,
            ),
        )
