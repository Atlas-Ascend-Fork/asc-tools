# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software: you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

import sqlite3
from dataclasses import dataclass
from typing import List, Dict, Tuple
from collections import defaultdict

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_enum import InstrType
from npu_emulate.src.backend.analyzers.pipe_dependency_analyzer import (
    PipeDependencyAnalyzer,
    PipeDependencyCategory,
)
from npu_emulate.src.backend.analyzers.david.pipe_dependency_flags_analyzer import (
    pipe_map,
)
from npu_emulate.src.common.extend_params import ExtendParams


@dataclass
class FFTSParams:
    instr_id: int
    mode: int
    tick: int
    pipe: str
    core_id: int
    core_type_id: int


class Ascend950PipeDependencyFFTSAnalyzer(PipeDependencyAnalyzer):
    # Cross-core FFTS (Fast Flag Transport Sync) dependency tracker.
    # Tracks SET_CROSS_CORE / WAIT_FLAG_DEVI instruction pairs that
    # synchronize execution across different cores via hardware flags.
    # Each SET instruction is paired with the next WAIT on the same flag_id.
    # Also updates InstrTypeId for SET/WAIT instructions to reflect their
    # actual pipe assignment (extracted from target_pipe in ExtendParams).
    def __init__(self) -> None:
        super().__init__()
        self.instrs_by_flag: Dict[int, List[FFTSParams]] = defaultdict(list)

    @staticmethod
    def get_name() -> str:
        return "ascend950_pipe_dependency_ffts"

    @staticmethod
    def get_set_flag_instrs() -> List[sqlite3.Row]:
        return database_handler.execute("""
            SELECT ExtendParams, ExecInstrId, ExecInstrTickStart, ExecInstrCoreId, CoreTypeId
            FROM ExecutedInstructions
            WHERE ExecInstrName LIKE '%SET_CROSS_CORE%'
            ORDER BY ExecInstrTickStart
            """).fetchall()

    @staticmethod
    def get_wait_flag_instrs() -> List[sqlite3.Row]:
        return database_handler.execute("""
            SELECT ExtendParams, ExecInstrId, ExecInstrCoreId, CoreTypeId
            FROM ExecutedInstructions
            WHERE ExecInstrName LIKE '%WAIT_FLAG_DEV%'
            ORDER BY ExecInstrTickStart
            """).fetchall()

    @staticmethod
    def get_flag_index(flag_id: int, pipe: int) -> int:
        ret = pipe << 4
        ret |= flag_id
        return ret

    @staticmethod
    def change_instr_type_id(instrs: List[Tuple[int, int]]) -> None:
        database_handler.executemany(
            """
            UPDATE ExecutedInstructions
            SET InstrTypeId=?
            WHERE ExecInstrId=?
            """,
            instrs,
        )

    def _find_instr_index(self, flag_id: int, instr: sqlite3.Row) -> int:
        exec_core_id = int(instr["ExecInstrCoreId"])
        core_type_id = int(instr["CoreTypeId"])
        for i, item in enumerate(self.instrs_by_flag[flag_id]):
            if item.core_id == exec_core_id and item.core_type_id == core_type_id:
                return i
        return -1

    def process_mode_0(self, flag_id: int, pipe: str, instr: sqlite3.Row) -> None:
        idx = self._find_instr_index(flag_id, instr)
        if idx == -1:
            return
        set_instr = self.instrs_by_flag[flag_id][idx]
        flag_index = self.get_flag_index(flag_id, pipe_map[set_instr.pipe])
        flow_event_id = self.get_flow_event_id(flag_index, set_instr.instr_id)
        flow_event_name = self.get_flow_event_name(set_instr.pipe, pipe, flag_id)
        self.store_pipe_dependency(
            set_instr.instr_id,
            flow_event_id,
            flow_event_name,
            False,
            PipeDependencyCategory.FFTS.value,
        )
        self.store_pipe_dependency(
            instr["ExecInstrId"],
            flow_event_id,
            flow_event_name,
            True,
            PipeDependencyCategory.FFTS.value,
        )
        self.instrs_by_flag[flag_id].pop(idx)

    @staticmethod
    def get_flow_event_name(set_pipe: str, wait_pipe: str, flag_id: int) -> str:
        return f"{wait_pipe} awaiting {set_pipe} flag id #{flag_id}"

    def process(self) -> None:
        set_instrs = self.get_set_flag_instrs()
        update_instr_type: List[Tuple[int, int]] = []
        for instr in set_instrs:
            ep = ExtendParams.parse(instr["ExtendParams"])
            flag_id = ep.ffts_flag_id
            pipe = ep.target_pipe
            assert flag_id is not None and pipe is not None, (
                f"Cannot extract FFTS flag_id/pipe from {dict(instr)} instruction."
            )
            update_instr_type.append(
                (
                    InstrType.PUSHQ.value if pipe == "VEC" else InstrType[pipe].value,
                    instr["ExecInstrId"],
                )
            )
            mode = ep.ffts_mode or 0
            params = FFTSParams(
                int(instr["ExecInstrId"]),
                mode,
                int(instr["ExecInstrTickStart"]),
                pipe,
                int(instr["ExecInstrCoreId"]),
                int(instr["CoreTypeId"]),
            )
            self.instrs_by_flag[flag_id].append(params)
        wait_instrs = self.get_wait_flag_instrs()
        for instr in wait_instrs:
            ep = ExtendParams.parse(instr["ExtendParams"])
            flag_id = ep.ffts_flag_id
            pipe = ep.target_pipe
            assert flag_id is not None and pipe is not None, (
                f"Cannot extract FFTS flag_id/pipe from {dict(instr)} instruction."
            )
            update_instr_type.append(
                (
                    InstrType.PUSHQ.value if pipe == "VEC" else InstrType[pipe].value,
                    instr["ExecInstrId"],
                )
            )
            if not self.instrs_by_flag[flag_id]:
                continue
            params = self.instrs_by_flag[flag_id][0]

            if params.mode == 0:
                self.process_mode_0(flag_id, pipe, instr)
        self.change_instr_type_id(update_instr_type)
