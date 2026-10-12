# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software: you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

from typing import List, Tuple, Optional, Iterator
from sqlite3 import Row

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer
from npu_emulate.src.common.extend_params import ExtendParams


ExecMaskItem = Tuple[int, int]


class DatabaseManager:
    @staticmethod
    def iter_instructions() -> Iterator[Row]:
        return database_handler.iter_rows("""
        SELECT ExecInstrId, ExtendParams
        FROM ExecutedInstructions
        """)

    @staticmethod
    def save_masks(batch: List[ExecMaskItem]) -> None:
        database_handler.executemany(
            """
        INSERT INTO ExecutionMasks (ExecInstrId, Mask)
        VALUES (?, ?)
        """,
            batch,
        )


class Ascend950MaskAnalyzer(IAnalyzer):
    # Computes the effective execution mask for each instruction.
    # The effective mask = predicate_mask & execution_mask (bitwise AND).
    # This determines which threads in a warp are actually active.
    # Skips instructions that don't have both prdct_mask and exec_mask params.
    BATCH_SIZE = 10_000

    @staticmethod
    def get_name() -> str:
        return "ascend950_mask_analyzer"

    @staticmethod
    def calc_mask(row: Row) -> Optional[ExecMaskItem]:
        ep = ExtendParams.parse(row["ExtendParams"])
        prdct_mask = ep.prdct_mask
        exec_mask = ep.exec_mask
        if prdct_mask is None or exec_mask is None:
            return None
        return (row["ExecInstrId"], prdct_mask & exec_mask)

    def process(self) -> None:
        batch: List[ExecMaskItem] = []
        for instr in DatabaseManager.iter_instructions():
            item = self.calc_mask(instr)
            if item is None:
                continue
            batch.append(item)
            if len(batch) >= self.BATCH_SIZE:
                DatabaseManager.save_masks(batch)
                batch.clear()
        if batch:
            DatabaseManager.save_masks(batch)
            batch.clear()
