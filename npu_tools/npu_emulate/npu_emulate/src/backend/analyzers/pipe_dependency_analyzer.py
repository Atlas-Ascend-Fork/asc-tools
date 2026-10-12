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

from enum import Enum, auto
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer
from npu_emulate.src.backend.database import database_handler


class PipeDependencyCategory(Enum):
    FLAGS = auto()
    INTRA_BLOCKS = auto()
    BUFFERS = auto()
    FFTS = auto()


class PipeDependencyAnalyzer(IAnalyzer):
    INSTR_ID_PAYLOAD_MASK = (1 << 40) - 1

    def __init__(self) -> None:
        super().__init__()

        max_instr = database_handler.execute(f"""
        SELECT COALESCE(
            MAX(ExecInstrId & {self.INSTR_ID_PAYLOAD_MASK}), 0
        ) as MaxID
        FROM ExecutedInstructions
        """).fetchone()

        self.MaxInstrID = int(max_instr["MaxID"]) + 1

    def get_flow_event_id(self, index: int, instr_id: int) -> int:
        # Use only bits 39..0; upper bits contain InstrTypeId and break logic
        # leading to SQLite INTEGER overflow error
        return (index + 1) * self.MaxInstrID + (instr_id & self.INSTR_ID_PAYLOAD_MASK)

    @staticmethod
    def store_pipe_dependency(
        instr_id: int,
        flow_event_id: int,
        flow_event_name: str,
        flow_end: bool,
        cat_id: int,
    ) -> None:
        database_handler.execute(
            f"""
            INSERT INTO PipeDependency
            VALUES ({",".join("?" * 5)})
            """,
            (instr_id, flow_event_id, flow_event_name, flow_end, cat_id),
        )
