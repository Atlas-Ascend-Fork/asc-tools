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

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.analyzers.base.memory_analyzer_base import (
    MemoryAnalyzerBase,
)


class Ascend950SIMTLdStAnalyzer(MemoryAnalyzerBase):
    def __init__(self) -> None:
        super().__init__()

    @staticmethod
    def get_name() -> str:
        return "ascend950_simt_ldg_stg_analyzer"

    def get_ldg_stg_instrs(self) -> List[sqlite3.Row]:
        executed_instrs = database_handler.execute("""
        SELECT *
        FROM SIMTExecutionMasks
        INNER JOIN ExecutedInstructions
        ON ExecutedInstructions.ExecInstrId=SIMTExecutionMasks.ExecInstrId
        WHERE
        ExecutedInstructions.ExecInstrName IN ("SIMT_LDG", "SIMT_STG",
                                               "SIMT_LDS", "SIMT_STS")
        ORDER BY ExecutedInstructions.ExecInstrId
        """)
        result: List[sqlite3.Row] = executed_instrs.fetchall()
        return result

    def process_instr(self, instr: sqlite3.Row) -> int:
        dtype = self.match_type(instr)
        dtype_size = self.get_dtype_size(dtype)
        processed_bytes = bin(instr["FullMask"]).count("1") * dtype_size
        return processed_bytes

    def add_simt_processed_bytes(
        self, instr_id: int, instr_name: str, core_type: int, processed_bytes: int
    ) -> None:
        values = [instr_id, instr_name, core_type, processed_bytes]
        database_handler.execute(
            f"""
            INSERT INTO SIMTLdStProcessedBytes
            VALUES ({",".join("?" * len(values))})
            """,
            values,
        )

    def process(self) -> None:
        instrs_list = self.get_ldg_stg_instrs()
        for instr in instrs_list:
            processed_bytes = self.process_instr(instr)
            self.add_simt_processed_bytes(
                instr["ExecInstrId"],
                instr["ExecInstrName"],
                instr["CoreTypeId"],
                processed_bytes,
            )
