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


class SingleCoreHint(IHint):
    # Kernel-level hint (ported from biprof "general hints"): fires when
    # ai_core_active <= 1, i.e. the kernel launched on at most one sub-core
    # type. Routes into the KernelHints table via is_kernel=True.
    #
    # Note: this overlaps with the existing SmallGridHint, which uses a
    # different signal (DISTINCT ExecInstrCoreId vs the architecture
    # maximum from core_num_map). Both are registered intentionally.

    @staticmethod
    def get_name() -> str:
        return "general_single_core"

    def instruction_condition(self, instr: sqlite3.Row) -> bool:
        return False

    def process(
        self, instrs_map: Dict[Optional[int], List[sqlite3.Row]]
    ) -> List[HintDataItem]:
        row = database_handler.execute(
            """
            SELECT KernelInfoVal FROM KernelInfo
            WHERE KernelInfoAttr = 'ai_core_active'
            """
        ).fetchone()
        if row and int(row[0]) <= 1:
            self.add_hint(
                "Single-core / under-launched kernel: only "
                f"{row[0]} core active. Query GetCoreNumAiv() or "
                "GetCoreNumAic() and launch on all cores. "
                "This is the dominant bottleneck — fix first.",
                [HintTarget(None, None, None, is_kernel=True)],
            )
        return self.result
