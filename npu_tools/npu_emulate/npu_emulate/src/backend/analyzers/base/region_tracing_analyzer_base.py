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

from dataclasses import dataclass
import sqlite3
from typing import List
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer


@dataclass
class Region:
    name: str
    start_time: int
    end_time: int
    core_id: int
    core_type_id: int
    region_type_id: int
    pipe_type_id: int
    warp_id: int


class RegionTracingAnalyzerBase(IAnalyzer):
    def __init__(self) -> None:
        super().__init__()

    def insert_region(self, region: Region) -> None:
        try:
            database_handler.execute(
                """
                INSERT INTO RegionNames
                (RegionName)
                VALUES(?)
            """,
                (region.name,),
            )
            region_name_id = database_handler.get_lastrowid()
        except sqlite3.IntegrityError:  # Region name already exists
            region_name_id = database_handler.execute(
                """
                SELECT RegionNameId
                FROM RegionNames
                WHERE RegionName=:name
                """,
                {"name": region.name},
            ).fetchone()[0]

        database_handler.execute(
            """
            INSERT INTO Regions
                (RegionNameId, RegionStart, RegionEnd,
                 RegionCoreId, CoreTypeId, RegionTypeId,
                 InstrQueueTypeId, WarpId)
            VALUES
                (:region_name_id, :region_start, :region_end,
                 :region_core_id, :core_type_id, :region_type_id,
                 :pipe_type_id, :warp_id)
        """,
            {
                "region_name_id": region_name_id,
                "region_start": region.start_time,
                "region_end": region.end_time,
                "region_core_id": region.core_id,
                "core_type_id": region.core_type_id,
                "region_type_id": region.region_type_id,
                "pipe_type_id": region.pipe_type_id,
                "warp_id": region.warp_id,
            },
        )

    def get_unique_cores(self) -> List[sqlite3.Row]:
        active = BackendDBManager.get_active_cores()
        result: List[sqlite3.Row] = []
        for core_id in active:
            row = database_handler.execute(
                """
            SELECT :id AS Id
            """,
                {"id": core_id},
            ).fetchone()
            if row is not None:
                result.append(row)
        return result

    def get_unique_core_types(self, core: int) -> List[sqlite3.Row]:
        unique_core_types = database_handler.execute(
            """
        SELECT DISTINCT CoreTypeId AS Id
        FROM ExecutedInstructions
        WHERE ExecInstrCoreId=:core
        """,
            {"core": core},
        )
        result: List[sqlite3.Row] = unique_core_types.fetchall()
        return result
