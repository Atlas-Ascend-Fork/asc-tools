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

from typing import Any

from npu_emulate.src.common.biprof_enum import InstrType
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer


class Ascend950ScalarInstructionPerCycleAnalyzer(IAnalyzer):
    @staticmethod
    def get_name() -> str:
        return "ascend950_scalar_ipc_analyzer"

    @staticmethod
    def make_indices() -> None:
        database_handler.execute("""
        CREATE INDEX IF NOT EXISTS idx_ei_scalar_ipc_covering
        ON ExecutedInstructions(
            ExecInstrCoreId, CoreTypeId,
            InstrTypeId, ExecInstrId, ExecInstrTickStart
        )
        """)
        database_handler.execute("""
        CREATE INDEX IF NOT EXISTS idx_dt_exec_instr_tick
        ON DispatchTime(ExecInstrId, DispatchTimeTick)
        """)

    @staticmethod
    def drop_indices() -> None:
        database_handler.execute("DROP INDEX IF EXISTS idx_ei_scalar_ipc_covering")
        database_handler.execute("DROP INDEX IF EXISTS idx_dt_exec_instr_tick")

    @staticmethod
    def get_stats(core_id: int, core_type_id: int) -> Any:
        return database_handler.execute(
            """
        SELECT COUNT() AS InstrCount,
        MAX(COALESCE(DispatchTimeTick, ExecInstrTickStart)) -
        MIN(COALESCE(DispatchTimeTick, ExecInstrTickStart))
        AS TickDifference FROM ExecutedInstructions
        LEFT JOIN DispatchTime
        ON ExecutedInstructions.ExecInstrId =
           DispatchTime.ExecInstrId
        WHERE ExecInstrCoreId = {}
        AND CoreTypeId = {}
        AND InstrTypeId NOT IN {}
        """.format(
                core_id,
                core_type_id,
                tuple(
                    [
                        InstrType.RVECEX.value,
                        InstrType.RVECLP.value,
                        InstrType.RVECSU.value,
                        InstrType.RVECLD.value,
                        InstrType.RVECST.value,
                    ]
                ),
            )
        ).fetchall()[0]

    @staticmethod
    def save_ipc_metric_value(ipc: float) -> None:
        database_handler.execute(
            """
        INSERT INTO KernelInfo (KernelInfoAttr, KernelInfoVal)
        VALUES
        (?, ?)
        """,
            ("scalar_ipc", ipc),
        )

    def process(self) -> None:
        self.make_indices()
        ipc_per_core = []
        for core_id in self.get_active_cores():
            ipc_per_subcore = []
            for core_type_id in self.get_active_core_types():
                stats = self.get_stats(core_id, core_type_id)
                dispatched_instrs_count = stats["InstrCount"]
                cycles = stats["TickDifference"]
                if not cycles:
                    continue
                ipc = dispatched_instrs_count / cycles
                ipc_per_subcore.append(ipc)
            ipc_per_core.append(sum(ipc_per_subcore) / len(ipc_per_subcore))
        ipc = round(sum(ipc_per_core) / len(ipc_per_core), 2)
        self.save_ipc_metric_value(ipc)
        self.drop_indices()
