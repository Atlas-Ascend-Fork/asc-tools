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
from npu_emulate.src.backend.hints.base.hint_base import IHint
from npu_emulate.src.common.biprof_constants import RELTIME_THRESHOLD


class SharedBankConflictsHint(IHint):
    @staticmethod
    def get_name() -> str:
        return "shared_bank_conflicts"

    def sourceline_condition(self, sourceline_instrs: List[sqlite3.Row]) -> bool:
        reltime_sum = 0
        for instr in sourceline_instrs:
            if instr["UBBankConflicts"]:
                reltime_sum += instr["RelTime"]
        return reltime_sum >= RELTIME_THRESHOLD

    @classmethod
    def get_sourceline_hint_text(cls, sourceline_instrs: List[sqlite3.Row]) -> str:
        return (
            "UB bank conflicts detected. "
            "Try to optimize local memory accesses, "
            "prefer sequential patterns instead of strided."
        )
