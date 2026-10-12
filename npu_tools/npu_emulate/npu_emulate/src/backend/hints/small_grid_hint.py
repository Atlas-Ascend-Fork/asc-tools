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
from npu_emulate.src.common.biprof_constants import core_num_map
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.common.biprof_enum import ArchName


class SmallGridHint(IHint):
    # Kernel-level hint: detects that the operator uses only a small
    # fraction of the physically available cores (a "small grid").
    # Active physical cores are derived from ExecutedInstructions
    # (DISTINCT ExecInstrCoreId), not from the KernelInfo.ai_core_active
    # attribute (which counts active sub-core types, not physical cores).
    # The hint triggers when active <= 50% of the architecture maximum
    # (e.g. <= 16 of 32 for V100).

    @staticmethod
    def get_name() -> str:
        return "small_grid_hint"

    def process(self, instrs_map: Dict[Optional[int], List]) -> List[HintDataItem]:
        arch_raw = biprof_ctx.arch
        if not arch_raw:
            arch_raw = database_handler.get_arch()
        if not arch_raw:
            return self.result
        try:
            arch = ArchName[str(arch_raw).upper()]
        except (KeyError, ValueError):
            return self.result
        max_cores = core_num_map.get(arch)
        if not max_cores:
            return self.result

        rows = database_handler.execute(
            "SELECT DISTINCT ExecInstrCoreId AS CoreId FROM ExecutedInstructions"
        ).fetchall()
        active = len(rows)
        if active <= max_cores // 2:
            self.add_hint(
                (
                    f"Small grid detected: only {active} of {max_cores} "
                    f"available cores are used. Consider increasing the "
                    f"block count to utilize more cores for better "
                    f"performance."
                ),
                [HintTarget(None, None, None, is_kernel=True)],
            )
        return self.result
