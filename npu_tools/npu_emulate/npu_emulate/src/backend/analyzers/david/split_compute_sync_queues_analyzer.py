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
from typing import List

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_constants import SYNCHRO_INSTRS
from npu_emulate.src.common.biprof_enum import InstrType, SubQueueType
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer


class Ascend950SplitComputeSyncQueuesAnalyzer(IAnalyzer):
    # Classifies instructions into sub-queue types for smart-pipes analysis.
    # Each instruction is assigned a SubQueueType:
    # - EXEC: regular compute instructions (CUBE, MTE, FIXP, PUSHQ)
    # - FLAG: synchronization instructions (SET_FLAG, WAIT_FLAG, GET_BUF, etc.)
    # - PUSH_PB: PUSH_PB instructions (special PUSHQ variant)
    # This classification enables separate utilization tracking for compute
    # vs synchronization queues in the pipeline.
    def __init__(self) -> None:
        super().__init__()

        self.supported_pipes: List[int] = [
            InstrType.CUBE.value,
            InstrType.MTE1.value,
            InstrType.MTE2.value,
            InstrType.MTE3.value,
            InstrType.FIXP.value,
            InstrType.PUSHQ.value,
        ]

    @staticmethod
    def get_name() -> str:
        return "ascend950_split_compute_sync_queues_analyzer"

    @staticmethod
    def make_index() -> None:
        database_handler.execute("""
        CREATE INDEX IF NOT EXISTS idx_ei_split_queues_covering
        ON ExecutedInstructions(
            ExecInstrCoreId, InstrTypeId, ExecInstrName, ExecInstrTickStart
        )
        """)

    @staticmethod
    def drop_index() -> None:
        database_handler.execute("DROP INDEX IF EXISTS idx_ei_split_queues_covering")

    def get_instrs_for_pipes(self, core_id: int) -> List[sqlite3.Row]:
        pipes_str = str(tuple(self.supported_pipes))
        return database_handler.execute(
            """
        SELECT ExecInstrId, InstrTypeId, ExecInstrName
        FROM ExecutedInstructions
        WHERE ExecInstrCoreId =:core_id
        AND (
            InstrTypeId IN {}
            OR ExecInstrName LIKE 'GET_BUF%'
            OR ExecInstrName LIKE 'RLS_BUF%'
            OR ExecInstrName LIKE 'SET_CROSS_CORE%'
            OR ExecInstrName LIKE 'WAIT_FLAG_DEV%'
        )
        ORDER BY ExecInstrTickStart
        """.format(pipes_str),
            {"core_id": core_id},
        ).fetchall()

    def save_subqueue_types(self, instrs: List[sqlite3.Row]) -> None:
        res_instrs = []
        for instr in instrs:
            if (
                int(instr["InstrTypeId"]) == InstrType.PUSHQ.value
                and "PUSH_PB" in instr["ExecInstrName"]
            ):
                sub_queue_type = SubQueueType.PUSH_PB.value
            else:
                sub_queue_type = (
                    SubQueueType.FLAG.value
                    if instr["ExecInstrName"] in SYNCHRO_INSTRS
                    else SubQueueType.EXEC.value
                )
            res_instrs.append((instr["ExecInstrId"], sub_queue_type))
        database_handler.executemany(
            """
            INSERT INTO SubQueueTypes
            (ExecInstrId, SubQueueType)
            VALUES
            (?, ?)
            """,
            res_instrs,
        )

    def process(self) -> None:
        self.make_index()
        for core_id in self.get_active_cores():
            instrs = self.get_instrs_for_pipes(core_id)
            self.save_subqueue_types(instrs)
        self.drop_index()
