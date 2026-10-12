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

from typing import List, Tuple, Iterator
from sqlite3 import Row

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer
from npu_emulate.src.common.extend_params import ExtendParams


SIMTExecMasksItem = Tuple[int, int, int, int, float]


class DatabaseManager:
    @staticmethod
    def iter_instructions() -> Iterator[Row]:
        return database_handler.iter_rows("""
        SELECT ExecInstrId, ExtendParams
        FROM ExecutedInstructions
        WHERE ExecInstrName LIKE '%SIMT%'
        """)

    @staticmethod
    def save_masks(data: List[SIMTExecMasksItem]) -> None:
        database_handler.executemany(
            """
        INSERT INTO SIMTExecutionMasks(
         ExecInstrId, PredicateMask,
         ExecutionMask, FullMask,
         BranchEfficiency)
        VALUES (?, ?, ?, ?, ?)
        """,
            data,
        )


class Ascend950BranchEfficiencyAnalyzer(IAnalyzer):
    # Branch efficiency = percentage of threads in a warp that take the
    # taken path. Computed from execution masks in SIMT instructions:
    #   full_mask = exec_mask & prdct_mask  (threads that executed)
    #   efficiency = count('1' bits in full_mask) / 32 * 100
    # A value of 100% means all threads agree (no divergence).
    #
    # With the contract DB, prdct_mask/exec_mask/branch_efficiency are
    # provided directly in ExtendParams.  When branch_efficiency is present
    # we use it as-is; otherwise we derive it from the masks.
    BATCH_SIZE = 10_000

    @staticmethod
    def get_name() -> str:
        return "ascend950_branch_efficiency_analyzer"

    @classmethod
    def iter_masks(
        cls, iter_instructions: Iterator[Row]
    ) -> Iterator[SIMTExecMasksItem]:
        for instr in iter_instructions:
            ep = ExtendParams.parse(instr["ExtendParams"])
            exec_mask = ep.exec_mask
            prdct_mask = ep.prdct_mask
            if exec_mask is None or prdct_mask is None:
                continue

            instr_id = int(instr["ExecInstrId"])
            full_mask = exec_mask & prdct_mask
            branch_efficiency = ep.branch_efficiency
            if branch_efficiency is None:
                branch_efficiency = 100 * (bin(full_mask).count("1") / 32)

            yield (
                instr_id,
                prdct_mask,
                exec_mask,
                full_mask,
                float(branch_efficiency),
            )

    def process(self) -> None:
        batch: List[SIMTExecMasksItem] = []
        iter_instructions = DatabaseManager.iter_instructions()
        for row in self.iter_masks(iter_instructions):
            batch.append(row)
            if len(batch) >= self.BATCH_SIZE:
                DatabaseManager.save_masks(batch)
                batch.clear()
        if batch:
            DatabaseManager.save_masks(batch)
            batch.clear()
