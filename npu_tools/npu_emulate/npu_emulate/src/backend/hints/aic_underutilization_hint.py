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
from npu_emulate.src.backend.hints.overlap_utils import (
    all_overlaps_below,
    compute_overlaps,
    get_ai_core_active,
    overlaps_to_string,
)
from npu_emulate.src.common.biprof_enum import CoreType, InstrQueueType


class AicUnderutilizationHint(IHint):
    # Kernel-level hint (ported from biprof "general hints"): fires when
    # the AIC CUBE pipe mean utilization is very low AND none of the AIC
    # pipeline overlaps with CUBE exceed a small threshold, indicating the
    # kernel barely does compute work with no pipeline overlap. Routes into
    # the KernelHints table via is_kernel=True.
    #
    # Compatible with npu_emulate data: the AIC pipe pairs (CUBE/MTE1/MTE2/
    # FIXP/SCALAR) all map to existing InstrQueueType / InstrType values,
    # and PipesUtilization is populated for these queue types.

    UTIL_THRESHOLD = 0.10
    OVERLAP_THRESHOLD = 0.10

    AIC_PIPE_PAIRS = [
        (("AIC", "MTE2"), ("AIC", "CUBE"), "AIC_MTE2_vs_AIC_CUBE"),
        (("AIC", "MTE1"), ("AIC", "CUBE"), "AIC_MTE1_vs_AIC_CUBE"),
        (("AIC", "FIXP"), ("AIC", "CUBE"), "AIC_FIXP_vs_AIC_CUBE"),
        (("AIC", "SCALAR"), ("AIC", "CUBE"), "AIC_SCALAR_vs_AIC_CUBE"),
    ]

    @staticmethod
    def get_name() -> str:
        return "general_aic_underutilization"

    def instruction_condition(self, instr: sqlite3.Row) -> bool:
        return False

    def process(
        self, instrs_map: Dict[Optional[int], List[sqlite3.Row]]
    ) -> List[HintDataItem]:
        ai_core_active = get_ai_core_active()
        if ai_core_active <= 1:
            return self.result

        cursor = database_handler.execute(
            """
            SELECT CoreId, CoreTypeId, InstrQueueTypeId, PipeUtilization
            FROM PipesUtilization
            """
        )
        pipe_values: List[float] = []
        for row in cursor.fetchall():
            core_type_id = row[1]
            pipe_type_id = row[2]
            util = row[3]
            core_type = CoreType(core_type_id).name
            pipe_type = InstrQueueType(pipe_type_id).name
            if core_type == "AIC" and pipe_type == "CUBE":
                pipe_values.append(util)
        if not pipe_values:
            return self.result
        mean_util = sum(pipe_values) / len(pipe_values) / 100
        mean_util = round(mean_util, 4)
        if mean_util >= self.UTIL_THRESHOLD:
            return self.result

        overlaps = compute_overlaps(self.AIC_PIPE_PAIRS)

        def aic_filter(k: str) -> bool:
            return k.startswith("AIC_") and k.endswith("_vs_AIC_CUBE")

        if not all_overlaps_below(overlaps, self.OVERLAP_THRESHOLD, aic_filter):
            return self.result

        overlap_str = overlaps_to_string(overlaps, aic_filter)
        self.add_hint(
            f"AIC underutilization: AIC_CUBE.mean={mean_util} "
            f"(threshold {self.UTIL_THRESHOLD}), all overlaps < "
            f"{self.OVERLAP_THRESHOLD} ({overlap_str}). The kernel barely "
            "does compute work with no pipeline overlap. Consider shrinking "
            "blockDim to match the work, using a representative shape, "
            "or shrinking baseM/baseN to manufacture more tiles.",
            [HintTarget(None, None, None, is_kernel=True)],
        )
        return self.result
