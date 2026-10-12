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


class VectorUtilizationHint(IHint):
    @staticmethod
    def get_name() -> str:
        return "vector_utilization_hint"

    def instruction_condition(self, instr: sqlite3.Row) -> bool:
        return (
            float(instr["RelTime"]) >= RELTIME_THRESHOLD
            and instr["VectorUtilization"] is not None
            and float(instr["VectorUtilization"]) <= 50.0
        )

    def get_instruction_hint_text(self, instr: sqlite3.Row) -> str:
        return (
            "Low vector utilization detected "
            f"({round(instr['VectorUtilization'], 2)}%). "
            "This can be a sign of inefficient "
            "vector instruction usage. "
            "Try to increase the number of processed elements "
            "and/or repeats for this vector instruction."
        )
