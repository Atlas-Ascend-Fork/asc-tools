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
from npu_emulate.src.backend.hints.base.hint_base import IHint
from npu_emulate.src.common.biprof_constants import RELTIME_THRESHOLD


class MTECallsHint(IHint):
    @staticmethod
    def get_name() -> str:
        return "mte_calls_hint"

    def instruction_condition(self, instr: sqlite3.Row) -> bool:
        params = instr["InstrParams"]
        return (
            instr["InstrName"].startswith("MOV")
            and params is not None
            and params.startswith("Src:")
            and float(instr["RelTime"]) >= RELTIME_THRESHOLD
            and int(instr["ExecutionCount"]) >= 16
            and int(instr["ProcessedBytes"]) <= 1024
        )

    def get_instruction_hint_text(self, instr: sqlite3.Row) -> str:
        return (
            "Inefficient memory copying detected. "
            "Consider using less data movement operations "
            "by moving data in bigger chunks."
        )
