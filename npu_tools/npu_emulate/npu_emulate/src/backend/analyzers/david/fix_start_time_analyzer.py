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
from typing import Dict, List

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.common.biprof_enum import InstrType
from npu_emulate.src.common.biprof_constants import SYNCHRO_INSTRS
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer


@dataclass
class Ticks:
    start: int
    end: int


class Ascend950FixStartTimeAnalyzer(IAnalyzer):
    # Fixes instruction start times for more accurate timing analysis.
    # Two corrections are applied:
    #   1. Flag/sync instructions (SET_FLAG, WAIT_FLAG, SET_INTRA_BLOCK, etc.)
    #      with duration=0 get start_time shifted back by 1 tick so they
    #      have a non-zero duration in the timeline.
    #   2. (smart_pipes mode only) For regular compute instructions, if
    #      start_tick < previous instruction's end_tick (pipeline overlap
    #      artifact), start_tick is pushed forward to prev_end_tick.
    def __init__(self) -> None:
        super().__init__()
        self.supported_pipes: List[InstrType] = [
            InstrType.CUBE,
            InstrType.PUSHQ,
            InstrType.FIXP,
            InstrType.MTE1,
            InstrType.MTE2,
            InstrType.MTE3,
        ]
        self.exec_instrs: Dict[int, Ticks] = {}

    @staticmethod
    def get_name() -> str:
        return "ascend950_fix_start_time_analyzer"

    @staticmethod
    def make_indices() -> None:
        database_handler.execute("""
            CREATE INDEX IF NOT EXISTS idx_ei_type_core_subcore_tickend
            ON ExecutedInstructions(
                InstrTypeId, ExecInstrCoreId, CoreTypeId,
                ExecInstrTickEnd, ExecInstrTickStart
            )
        """)
        database_handler.execute("""
            CREATE INDEX IF NOT EXISTS idx_ei_zero_dur_name
            ON ExecutedInstructions(ExecInstrName)
            WHERE ExecInstrTickEnd = ExecInstrTickStart
        """)

    @staticmethod
    def drop_indices() -> None:
        database_handler.execute(
            "DROP INDEX IF EXISTS idx_ei_type_core_subcore_tickend"
        )
        database_handler.execute("DROP INDEX IF EXISTS idx_ei_zero_dur_name")

    def update_tick_starts_for_flag(self) -> None:
        database_handler.execute("""
            UPDATE ExecutedInstructions
            SET ExecInstrTickStart = ExecInstrTickStart - 1
            WHERE ExecInstrTickEnd = ExecInstrTickStart
            AND (
                ExecInstrName LIKE 'SET_INTRA_BLOCK%'
                OR ExecInstrName LIKE 'WAIT_INTRA_BLOCK%'
                OR ExecInstrName LIKE 'SET_FLAG%'
                OR ExecInstrName LIKE 'WAIT_FLAG%'
                OR ExecInstrName LIKE 'GET_BUF%'
                OR ExecInstrName LIKE 'RLS_BUF%'
                OR ExecInstrName LIKE 'SET_CROSS_CORE%'
            )
            """)

    @staticmethod
    def get_instrs_for_exec(
        pipe: InstrType, core_id: int, core_type_id: int
    ) -> List[sqlite3.Row]:
        return database_handler.execute(
            """
        SELECT * FROM ExecutedInstructions
        WHERE ExecInstrCoreId = :core_id
        AND CoreTypeId = :core_type_id
        AND InstrTypeId = :instr_type_id
        ORDER BY ExecInstrTickEnd, ExecInstrTickStart
        """,
            {
                "instr_type_id": pipe.value,
                "core_id": core_id,
                "core_type_id": core_type_id,
            },
        ).fetchall()

    def update_start_time_for_exec(self, instrs: List[sqlite3.Row]) -> None:
        res_instrs: Dict[int, Ticks] = {}
        if not instrs:
            return
        prev_end_tick = instrs[0]["ExecInstrTickEnd"]
        for instr in instrs[1:]:
            if (
                instr["ExecInstrName"] in SYNCHRO_INSTRS
                or instr["ExecInstrName"] == "PUSH_PB"
            ):
                continue
            current_end = instr["ExecInstrTickEnd"]
            if instr["ExecInstrTickStart"] < prev_end_tick:
                res_instrs[instr["ExecInstrId"]] = Ticks(prev_end_tick, current_end)
            prev_end_tick = current_end
        self.exec_instrs.update(res_instrs)

    def update_tick_starts_for_exec(self) -> None:
        database_handler.executemany(
            """
            UPDATE ExecutedInstructions
            SET ExecInstrTickStart=?,
                ExecInstrTickEnd=?
            WHERE ExecInstrId=?
            """,
            [(val.start, val.end, key) for key, val in self.exec_instrs.items()],
        )

    def process(self) -> None:
        self.make_indices()
        self.update_tick_starts_for_flag()
        if not biprof_ctx.smart_pipes:
            return
        core_type_ids = self.get_active_core_types()
        for core_id in self.get_active_cores():
            for core_type_id in core_type_ids:
                for pipe in self.supported_pipes:
                    exec_instrs = self.get_instrs_for_exec(pipe, core_id, core_type_id)
                    self.update_start_time_for_exec(exec_instrs)
        self.update_tick_starts_for_exec()
        self.drop_indices()

    def cleanup(self) -> None:
        self.exec_instrs.clear()
