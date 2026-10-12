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
from typing import Dict, Iterator, List

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_enum import InstrType
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer


class Ascend950VFMemoryAccessAnalyzer(IAnalyzer):
    # Maps SIMD memory accesses to their parent VF instruction.
    # Scans VF instructions and their inner load/store (RVECLD/RVECST) ops,
    # then for each load/store, finds which VF instruction's tick range
    # contains it. Saves the mapping so memory accesses can be attributed
    # to specific VF instructions in downstream analysis/reports.
    BATCH_SIZE = 10000

    def __init__(self) -> None:
        super().__init__()
        self.instr_to_vf: Dict[int, int] = {}

    @staticmethod
    def get_name() -> str:
        return "ascend950_vf_memory_access_analyzer"

    @staticmethod
    def get_vf_instrs(core_id: int, core_type_id: int) -> List[sqlite3.Row]:
        return database_handler.execute(
            """
        SELECT * FROM ExecutedInstructions
        WHERE ExecutedInstructions.ExecInstrName = "VF"
        AND ExecutedInstructions.ExecInstrCoreId = :core_id
        AND ExecutedInstructions.CoreTypeId = :core_type_id
        ORDER BY ExecInstrTickStart
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchall()

    @staticmethod
    def iter_vf_ld_st_instrs(core_id: int, core_type_id: int) -> Iterator[sqlite3.Row]:
        return database_handler.iter_rows(
            f"""
        SELECT * FROM ExecutedInstructions
        WHERE ExecutedInstructions.ExecInstrCoreId = :core_id
        AND ExecutedInstructions.CoreTypeId = :core_type_id
        AND (InstrTypeId = {InstrType.RVECLD.value}
             OR InstrTypeId = {InstrType.RVECST.value})
        ORDER BY ExecInstrTickStart
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        )

    @staticmethod
    def iter_vf_mem_accesses(core_id: int, core_type_id: int) -> Iterator[sqlite3.Row]:
        return database_handler.iter_rows(
            f"""
        SELECT * FROM MemoryRWAccesses
        LEFT JOIN ExecutedInstructions
        ON ExecutedInstructions.ExecInstrId=MemoryRWAccesses.ExecInstrId
        WHERE CoreId=:core_id
        AND CoreTypeId = :core_type_id
        AND (InstrTypeId = {InstrType.RVECLD.value}
             OR InstrTypeId = {InstrType.RVECST.value})
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        )

    def process_instrs(
        self, vf_instrs: List[sqlite3.Row], ld_st_instrs: Iterator[sqlite3.Row]
    ) -> None:
        for instr in ld_st_instrs:
            for vf in vf_instrs:
                if (
                    vf["ExecInstrTickStart"]
                    <= instr["ExecInstrTickStart"]
                    <= vf["ExecInstrTickEnd"]
                    and vf["ExecInstrTickStart"]
                    <= instr["ExecInstrTickEnd"]
                    <= vf["ExecInstrTickEnd"]
                ):
                    self.instr_to_vf[instr["ExecInstrId"]] = vf["ExecInstrId"]
                    break

    def save_vf_mem_accesses(self, mem_accesses: Iterator[sqlite3.Row]) -> None:
        insert_sql = """
            INSERT INTO VFMemoryRWAccesses
            (AccessedAddress, AccessMode, AccessTime,
             MemoryType, ExecInstrId, CoreId)
            VALUES
            (?, ?, ?, ?, ?, ?)
            """
        batch: List[tuple] = []
        for item in mem_accesses:
            if item["ExecInstrId"] not in self.instr_to_vf:
                continue
            batch.append(
                (
                    item["AccessedAddress"],
                    item["AccessMode"],
                    item["AccessTime"],
                    item["MemoryType"],
                    self.instr_to_vf[item["ExecInstrId"]],
                    item["CoreId"],
                )
            )
            if len(batch) >= self.BATCH_SIZE:
                database_handler.executemany(insert_sql, batch)
                batch.clear()
        if batch:
            database_handler.executemany(insert_sql, batch)

    def process(self) -> None:
        for core_id in self.get_active_cores():
            for core_type_id in self.get_active_core_types():
                vf_instrs = self.get_vf_instrs(core_id, core_type_id)
                if not vf_instrs:
                    continue
                ld_st_instrs = self.iter_vf_ld_st_instrs(core_id, core_type_id)
                self.process_instrs(vf_instrs, ld_st_instrs)
                mem_accesses = self.iter_vf_mem_accesses(core_id, core_type_id)
                self.save_vf_mem_accesses(mem_accesses)

    def cleanup(self) -> None:
        self.instr_to_vf.clear()
