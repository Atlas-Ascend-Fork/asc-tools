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


class LoadImbalanceHint(IHint):
    # Kernel-level hint (ported from biprof "general hints"): fires when
    # the per-core kernel-duration ratio (max/min) exceeds a mild or
    # severe threshold, indicating uneven work distribution across cores.
    # Routes into the KernelHints table via is_kernel=True.
    #
    # Note: this overlaps with the existing TailEffectHint, which uses a
    # relative-gap formulation ((max-min)/max > 0.30) instead of a ratio.
    # Both are registered intentionally.

    MILD_THRESHOLD = 1.3
    SEVERE_THRESHOLD = 2.0

    @staticmethod
    def get_name() -> str:
        return "general_load_imbalance"

    def instruction_condition(self, instr: sqlite3.Row) -> bool:
        return False

    def process(
        self, instrs_map: Dict[Optional[int], List[sqlite3.Row]]
    ) -> List[HintDataItem]:
        cursor = database_handler.execute(
            """
            SELECT ExecInstrCoreId,
                MAX(ExecInstrTickEnd) - MIN(ExecInstrTickStart)
                AS KernelDuration
            FROM ExecutedInstructions
            GROUP BY ExecInstrCoreId
            """
        )
        durations = [row[1] for row in cursor.fetchall()]
        if len(durations) < 2:
            return self.result
        ratio = round(max(durations) / min(durations), 4)
        if ratio > self.SEVERE_THRESHOLD:
            self.add_hint(
                f"Severe multi-core load imbalance: imbalance_ratio={ratio} "
                f"(threshold {self.SEVERE_THRESHOLD}). Cores have uneven work "
                "distribution. Check block dim, remainder distribution, or "
                "non-uniform data structure.",
                [HintTarget(None, None, None, is_kernel=True)],
            )
        elif ratio > self.MILD_THRESHOLD:
            self.add_hint(
                f"Mild multi-core load imbalance: imbalance_ratio={ratio} "
                f"(threshold {self.MILD_THRESHOLD}). Some cores finish early "
                "while others are still running. Check remainder elements "
                "distribution or block dim.",
                [HintTarget(None, None, None, is_kernel=True)],
            )
        return self.result
