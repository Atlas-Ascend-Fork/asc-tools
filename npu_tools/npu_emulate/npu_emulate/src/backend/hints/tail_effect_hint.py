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

from typing import Dict, List, Optional

from npu_emulate.src.backend.hints.base.hint_base import (
    HintDataItem,
    HintTarget,
    IHint,
)
from npu_emulate.src.backend.database import database_handler


class TailEffectHint(IHint):
    # Kernel-level hint: detects a "tail effect" — non-uniform job
    # distribution across the active physical cores, where some cores
    # finish much earlier than others and sit idle while the busiest
    # core is still running.
    #
    # Per-core span is computed as MAX(ExecInstrTickEnd) -
    # MIN(ExecInstrTickStart) grouped by physical core (ExecInstrCoreId).
    # The hint triggers when the relative gap between the busiest and
    # the fastest core exceeds 30% of the busiest core's span:
    #     (max_span - min_span) / max_span > 0.30
    # At least 2 active cores are required; with a single core there is
    # no tail effect to detect.

    TAIL_THRESHOLD = 0.30

    @staticmethod
    def get_name() -> str:
        return "tail_effect_hint"

    def process(self, instrs_map: Dict[Optional[int], List]) -> List[HintDataItem]:
        rows = database_handler.execute(
            "SELECT ExecInstrCoreId AS CoreId, "
            "MIN(ExecInstrTickStart) AS TickStart, "
            "MAX(ExecInstrTickEnd) AS TickEnd "
            "FROM ExecutedInstructions GROUP BY CoreId"
        ).fetchall()
        if len(rows) < 2:
            return self.result
        spans = [int(r["TickEnd"]) - int(r["TickStart"]) for r in rows]
        max_span = max(spans)
        min_span = min(spans)
        if max_span <= 0:
            return self.result
        rel_gap = (max_span - min_span) / max_span
        if rel_gap > self.TAIL_THRESHOLD:
            self.add_hint(
                (
                    f"Tail effect detected: job distribution across cores "
                    f"is non-uniform (the busiest core runs "
                    f"{round(rel_gap * 100)}% longer than the fastest). "
                    f"Consider balancing the workload across cores to "
                    f"improve utilization."
                ),
                [HintTarget(None, None, None, is_kernel=True)],
            )
        return self.result
