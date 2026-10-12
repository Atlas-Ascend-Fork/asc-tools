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
from typing import List, Optional, Iterator
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer


@dataclass
class VecUtilEntry:
    exec_instr_id: int
    processed_bytes: int
    processed_elements: Optional[int]
    vec_util_value: float


class VectorUtilizationAnalyzerBase(IAnalyzer):
    def __init__(self) -> None:
        super().__init__()
        self.utilizations: List[VecUtilEntry] = []

    @staticmethod
    def get_name() -> str:
        return "vector_utilization"

    @staticmethod
    def iter_vec_instrs() -> Iterator[sqlite3.Row]:
        raise NotImplementedError

    @staticmethod
    def is_supported_instr(instr: sqlite3.Row) -> bool:
        raise NotImplementedError

    def calc_utilization(self, instr: sqlite3.Row) -> Optional[VecUtilEntry]:
        raise NotImplementedError

    def save_utilizations(self) -> None:
        database_handler.executemany(
            """
            INSERT INTO VectorUtilizations
            (ExecInstrId,
             ProcessedBytes,
             ProcessedElements,
             VectorUtilization)
            VALUES
            (?, ?, ?, ?)
        """,
            [
                (
                    entry.exec_instr_id,
                    entry.processed_bytes,
                    entry.processed_elements,
                    entry.vec_util_value,
                )
                for entry in self.utilizations
            ],
        )

    def process(self) -> None:
        for instr in self.iter_vec_instrs():
            if not self.is_supported_instr(instr):
                continue
            vec_util_entry = self.calc_utilization(instr)
            if vec_util_entry:
                self.utilizations.append(vec_util_entry)
        self.save_utilizations()

    def cleanup(self) -> None:
        self.utilizations.clear()
