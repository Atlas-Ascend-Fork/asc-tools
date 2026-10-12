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
import logging as log
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
class DMAMovLoad2dParamsInfo(DMAParamsInfo):
    m_start_pos: int
    k_start_pos: int
    m_step: int
    k_step: int


class Ascend950DMAMovLoad2DAnalyzer(DMAMovAnalyzerBase):
    def __init__(self) -> None:
        super().__init__()
        self.instr_params["LOAD_2Dv2"] = {}
        self.instr_params["LOAD_MX_2Dv2"] = {}

    @staticmethod
    def get_name() -> str:
        return "ascend950_dma_mov_load_2d_analyzer"

    def process_instruction(self, instr: sqlite3.Row) -> Optional[DMAInstrInfo]:
        ep = ExtendParams.parse(instr["ExtendParams"])
        if ep.src_mem != "L1":
            log.warning(f"LOAD_2Dv2 instruction has unsupported src_mem: {ep.src_mem}")
            return None
        m_start_pos = ep.m_start_pos
        k_start_pos = ep.k_start_pos
        m_step = ep.m_step
        k_step = ep.k_step
        processed_bytes = ep.processed_bytes
        if any(v is None for v in (m_start_pos, k_start_pos, m_step, k_step)):
            return None
        return DMAInstrInfo(
            DMAMovLoad2dParamsInfo(m_start_pos, k_start_pos, m_step, k_step),
            processed_bytes,
        )

    @classmethod
    def save_dma_params_info(cls, instr_id: int, params_info: DMAParamsInfo) -> None:
        assert isinstance(params_info, DMAMovLoad2dParamsInfo)
        database_handler.execute(
            """
            INSERT INTO DmaMovLoad2dParams
            (ExecInstrId,
             MStartPosition,
             KStartPosition,
             MStep,
             KStep)
            VALUES
            (?, ?, ?, ?, ?)
        """,
            (
                instr_id,
                params_info.m_start_pos,
                params_info.k_start_pos,
                params_info.m_step,
                params_info.k_step,
            ),
        )
