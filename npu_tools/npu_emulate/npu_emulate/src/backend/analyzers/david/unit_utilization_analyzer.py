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
from typing import List
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_enum import CoreType, InstrType, SubQueueType
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer
from npu_emulate.src.common.biprof_context import biprof_ctx

POS_START = 0
POS_END = 1


@dataclass
class UnitUtilEvent:
    tick: int
    util: float
    core_id: int
    core_type_id: int


class Ascend950UnitUtilizationAnalyzer(IAnalyzer):
    # Unit utilization = fraction of compute pipes actively executing
    # at each point in time (event-counting approach).
    #
    # Algorithm:
    #   1. Collect instruction START (Pos=0) and END (Pos=1) events per pipe.
    #   2. Merge + sort by tick to create a unified timeline.
    #   3. At each event, track which pipe types are "in flight".
    #   4. Utilization = |unique active pipes| / |total pipes for this core|.
    #
    #   +-------+          +---+
    #   | CUBE  |          |FIX|
    #   +-------+  +-----+ +---+      3 active -> 75% (of 4 pipes)
    #              | MTE |             1 active -> 25%
    #              +-----+
    #   tick0   tick1  tick2 tick3 ...
    aiv_pipes = [InstrType.MTE2, InstrType.MTE3, InstrType.PUSHQ]
    units_parts = {
        CoreType.AIC: [
            InstrType.CUBE,
            InstrType.FIXP,
            InstrType.MTE1,
            InstrType.MTE2,
        ],
        CoreType.AIV0: aiv_pipes,
        CoreType.AIV1: aiv_pipes,
    }

    @staticmethod
    def get_name() -> str:
        return "ascend950_unit_utilization_analyzer"

    @staticmethod
    def _get_instr_type_id(core_type_id: int) -> str:
        parts = Ascend950UnitUtilizationAnalyzer.units_parts[CoreType(core_type_id)]
        values = tuple(part.value for part in parts)
        return str(values)

    def get_instrs(self, core_id: int, core_type_id: int) -> List[sqlite3.Row]:
        # Split instructions into start and end events, then merge-sort by tick
        # smart_pipes mode additionally filters by SubQueueType=EXEC to
        # distinguish actual execution from other sub-queue stages.
        instr_type_id = self._get_instr_type_id(core_type_id)
        start_query = (
            """
            SELECT ExecInstrTickStart AS Tick,
                   InstrTypeId, 0 AS Pos FROM ExecutedInstructions
            WHERE ExecInstrCoreId=:core_id
            AND CoreTypeId=:core_type_id
            AND InstrTypeId IN {}
            ORDER BY ExecInstrTickStart
            """.format(instr_type_id)
            if not biprof_ctx.smart_pipes
            else """
            SELECT ExecInstrTickStart AS Tick,
                   InstrTypeId, 0 AS Pos FROM ExecutedInstructions
            LEFT JOIN SubQueueTypes
            ON SubQueueTypes.ExecInstrId = ExecutedInstructions.ExecInstrId
            WHERE ExecInstrCoreId=:core_id
            AND CoreTypeId=:core_type_id
            AND InstrTypeId IN {}
            AND SubQueueType={}
            ORDER BY ExecInstrTickStart
            """.format(instr_type_id, SubQueueType.EXEC.value)
        )
        starts = database_handler.execute(
            start_query, {"core_id": core_id, "core_type_id": core_type_id}
        ).fetchall()
        end_query = (
            """
            SELECT ExecInstrTickEnd AS Tick,
                   InstrTypeId, 1 As Pos FROM ExecutedInstructions
            WHERE ExecInstrCoreId=:core_id
            AND CoreTypeId=:core_type_id
            AND InstrTypeId IN {}
            ORDER BY ExecInstrTickEnd
            """.format(instr_type_id)
            if not biprof_ctx.smart_pipes
            else """
            SELECT ExecInstrTickEnd AS Tick,
                   InstrTypeId, 1 As Pos FROM ExecutedInstructions
            LEFT JOIN SubQueueTypes
            ON SubQueueTypes.ExecInstrId = ExecutedInstructions.ExecInstrId
            WHERE ExecInstrCoreId=:core_id
            AND CoreTypeId=:core_type_id
            AND InstrTypeId IN {}
            AND SubQueueType={}
            ORDER BY ExecInstrTickEnd
            """.format(instr_type_id, SubQueueType.EXEC.value)
        )
        ends = database_handler.execute(
            end_query, {"core_id": core_id, "core_type_id": core_type_id}
        ).fetchall()
        res = starts + ends
        # Merge-sort by tick: start events (Pos=0) and end events (Pos=1)
        # from all pipes are interleaved into one chronological timeline.
        res.sort(key=lambda x: int(x["Tick"]))
        return res

    @staticmethod
    def save_events(res: List[UnitUtilEvent]) -> None:
        database_handler.executemany(
            """
            INSERT INTO UnitUtilization
            (Tick, Util, CoreId, CoreTypeId)
            VALUES
            (?, ?, ?, ?)
            """,
            [(item.tick, item.util, item.core_id, item.core_type_id) for item in res],
        )

    def process(self) -> None:
        res = []
        for core_id in self.get_active_cores():
            for core_type_id in self.get_active_core_types():
                instrs = self.get_instrs(core_id, core_type_id)
                current_pipes: List[str] = []
                # Walk the timeline: add pipe on START, remove on END.
                # set() deduplicates same-pipe concurrent instructions.
                for instr in instrs:
                    if int(instr["Pos"]) == POS_START:
                        current_pipes.append(InstrType(int(instr["InstrTypeId"])).name)
                    elif int(instr["Pos"]) == POS_END:
                        pipe_name = InstrType(int(instr["InstrTypeId"])).name
                        if pipe_name in current_pipes:
                            current_pipes.remove(pipe_name)
                    util = len(set(current_pipes)) / len(
                        self.units_parts[CoreType(core_type_id)]
                    )
                    res.append(
                        UnitUtilEvent(instr["Tick"], util, core_id, core_type_id)
                    )
        self.save_events(res)
