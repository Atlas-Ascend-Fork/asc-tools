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
from npu_emulate.src.common.biprof_constants import WAIT_INSTRS
from npu_emulate.src.backend.analyzers.pipe_utilization_analyzer import (
    PipeUtilizationAnalyzer,
    PipeUtilInfo,
)
import npu_emulate.src.common.biprof_enum as enums

InstrQueueType = enums.InstrQueueType
queue_type_to_instr_types = enums.queue_type_to_instr_types()


class Ascend950PipeUtilizationAnalyzer(PipeUtilizationAnalyzer):
    def __init__(self) -> None:
        super().__init__()

    @staticmethod
    def get_name() -> str:
        return "ascend950_pipe_utilization"

    @staticmethod
    def get_simd_or_simt_pipe_instrs(
        core_id: int, core_type_id: int, is_simt: bool
    ) -> List[sqlite3.Row]:
        if is_simt:
            queue_type = InstrQueueType.SIMT
            optional_not = ""
        else:
            queue_type = InstrQueueType.SIMD
            optional_not = "NOT"

        executed_vec_instrs = None
        instr_type_ids = [e.value for e in queue_type_to_instr_types[queue_type]]
        instr_types_str = f"({', '.join(map(str, instr_type_ids))})"

        executed_vec_instrs = database_handler.execute(
            """
            SELECT InstrTypeId, ExecInstrName,
                   ExecInstrTickStart, ExecInstrTickEnd
            FROM ExecutedInstructions
            WHERE ExecInstrCoreId = :core_id AND CoreTypeId = :core_type_id
                  AND InstrTypeId IN {0}
                  AND ExecInstrName NOT IN {1}
                  AND ExecInstrName {2} LIKE 'SIMT%'
            ORDER BY ExecInstrTickStart
        """.format(instr_types_str, WAIT_INSTRS, optional_not),
            {"core_id": core_id, "core_type_id": core_type_id},
        )

        result: List[sqlite3.Row] = executed_vec_instrs.fetchall()
        return result

    def process_single_pipe(
        self, core_id: int, core_type_id: int, instr_queue_type: InstrQueueType
    ) -> PipeUtilInfo:
        if instr_queue_type == InstrQueueType.SIMD:
            pipe_instrs = self.get_simd_or_simt_pipe_instrs(
                core_id, core_type_id, is_simt=False
            )
        elif instr_queue_type == InstrQueueType.SIMT:
            pipe_instrs = self.get_simd_or_simt_pipe_instrs(
                core_id, core_type_id, is_simt=True
            )
        else:
            pipe_instrs = self.get_pipe_instrs(core_id, core_type_id, instr_queue_type)
        utilization_value = self.calc_utilization(
            pipe_instrs, self.kernel_exec_time_ticks
        )
        return PipeUtilInfo(
            core_id=core_id,
            core_type_id=core_type_id,
            instr_queue_type=instr_queue_type,
            utilization_value=utilization_value,
        )
