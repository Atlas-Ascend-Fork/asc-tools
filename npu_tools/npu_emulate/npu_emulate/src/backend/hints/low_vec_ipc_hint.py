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
from typing import Dict, List, Optional

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.hints.base.hint_base import (
    HintDataItem,
    HintTarget,
    IHint,
)


class LowVecIpcHint(IHint):
    # Kernel-level hint: fires when the mean ExecIPC across VF (vector)
    # instructions is low, indicating that the VEC pipe is
    # instruction-starved (low instruction-per-clock ratio on VEC).
    #
    # The VfIPC table holds per-instruction IPC counters for VF (PUSHQ)
    # instructions on AIV cores. No InstrType/CoreType filtering is needed
    # because VfIPC only contains rows for those instructions.
    #
    # Distinct from VectorUtilizationHint, which is a per-instruction hint
    # based on the VectorUtilization column (vector lane utilization).
    # This hint aggregates VfIPC.ExecIPC across the whole kernel to flag
    # a kernel-level VEC IPC problem.
    #
    # Routes into the KernelHints table via is_kernel=True.

    IPC_THRESHOLD = 0.5

    @staticmethod
    def get_name() -> str:
        return "low_vec_ipc_hint"

    def instruction_condition(self, instr: sqlite3.Row) -> bool:
        return False

    def process(
        self, instrs_map: Dict[Optional[int], List[sqlite3.Row]]
    ) -> List[HintDataItem]:
        cursor = database_handler.execute("SELECT ExecIPC FROM VfIPC")
        rows = cursor.fetchall()
        if not rows:
            return self.result
        ipc_values = [float(r[0]) for r in rows if r[0] is not None]
        if not ipc_values:
            return self.result
        mean_ipc = round(sum(ipc_values) / len(ipc_values), 4)
        if mean_ipc >= self.IPC_THRESHOLD:
            return self.result

        self.add_hint(
            f"Low VEC IPC: mean ExecIPC={mean_ipc} across {len(ipc_values)} "
            f"VF instructions (threshold {self.IPC_THRESHOLD}). The vector "
            "pipe is instruction-starved. Consider increasing the vector "
            "instruction count, reducing stalls, or improving "
            "instruction-level parallelism.",
            [HintTarget(None, None, None, is_kernel=True)],
        )
        return self.result
