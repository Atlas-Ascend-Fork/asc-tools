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
from itertools import groupby
from typing import List, Dict, Tuple, Iterator, Iterable
from collections import defaultdict
from npu_emulate.src.common.biprof_constants import (
    MEMORY_ACCESS_READ,
    SUPPORTED_MEM_TYPES,
)
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_enum import MemoryType
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer


@dataclass
class UBAccess:
    mode: str
    tick: int

    def __hash__(self):
        return hash(self.tick)


@dataclass
class UBMemoryEvent:
    tick: int
    bytes_operation: int


@dataclass
class MemStateEvent:
    core_id: int
    tick: int
    bytes: int
    mem_type: MemoryType


class Ascend950MemoryUtilizationAnalyzer(IAnalyzer):
    # Memory utilization tracker: measures per-address write-to-read lifetimes.
    # For each memory address in UB/L0A/L0B/L0C/L1, tracks when data is
    # written (W) and when it's subsequently read (R). The time between
    # write and read represents how long the data occupies memory (lifetime).
    # Aggregates into utilization snapshots at each event tick, showing
    # how many bytes are actively in use per memory type over time.
    TIME_INTERVAL = 100
    RW_LOG_CELL_LEN = 4
    BATCH_SIZE = 10_000

    def __init__(self) -> None:
        super().__init__()
        self.supported_mem_types = SUPPORTED_MEM_TYPES

    @staticmethod
    def get_name() -> str:
        return "ascend950_memory_utilization"

    @staticmethod
    def store_UB_states(ub_states: List[MemStateEvent]) -> None:
        database_handler.executemany(
            """
            INSERT INTO MemoryUtilizationStates
            (CoreId, Tick, UtilizedBytes, MemoryType)
            VALUES (?, ?, ?, ?)
        """,
            [(i.core_id, i.tick, i.bytes, i.mem_type.value) for i in ub_states],
        )

    @staticmethod
    def iter_accesses(
        core_id: int, mem_type: MemoryType
    ) -> Iterator[Tuple[int, List[UBAccess]]]:
        rows = database_handler.iter_rows(
            """
        SELECT MemoryRWAccesses.AccessedAddress AS Addr,
            MemoryRWAccesses.AccessMode AS Mode,
            MemoryRWAccesses.AccessTime AS Tick,
            ExecutedInstructions.ExecInstrTickEnd as TickEnd
        FROM MemoryRWAccesses
        LEFT JOIN ExecutedInstructions
        ON ExecutedInstructions.ExecInstrId=MemoryRWAccesses.ExecInstrId
        WHERE CoreId=:core_id AND
            MemoryType=:memory_type
        ORDER BY MemoryRWAccesses.AccessedAddress, MemoryRWAccesses.AccessTime
        """,
            {"core_id": core_id, "memory_type": mem_type.value},
        )

        for addr, addr_rows in groupby(rows, key=lambda r: r["Addr"]):
            yield (
                addr,
                [
                    UBAccess(r["Mode"], r["Tick"] if r["Mode"] == "W" else r["TickEnd"])
                    for r in addr_rows
                ],
            )

    @classmethod
    def get_memory_events(
        cls, access_events: Iterable[Tuple[int, List[UBAccess]]]
    ) -> List[UBMemoryEvent]:
        mem_events: Dict[int, int] = defaultdict(int)
        for _, events in access_events:
            last_read_tick = None
            last_write_tick = None
            for event in sorted(events, key=lambda x: x.tick):
                if event.mode == MEMORY_ACCESS_READ:
                    last_read_tick = event.tick
                else:
                    if last_read_tick is not None and last_write_tick is not None:
                        mem_events[last_write_tick] += cls.RW_LOG_CELL_LEN
                        mem_events[last_read_tick] -= cls.RW_LOG_CELL_LEN
                        last_write_tick = None
                    last_read_tick = None
                    last_write_tick = (
                        event.tick if not last_write_tick else last_write_tick
                    )
            if last_read_tick is not None and last_write_tick is not None:
                mem_events[last_write_tick] += cls.RW_LOG_CELL_LEN
                mem_events[last_read_tick] -= cls.RW_LOG_CELL_LEN
        res = [UBMemoryEvent(tick, op) for tick, op in mem_events.items()]
        return sorted(res, key=lambda x: x.tick)

    def get_memory_states(
        self, core_id: int, mem_type_access_map: Dict[MemoryType, List[UBMemoryEvent]]
    ) -> Iterator[MemStateEvent]:
        state_map: Dict[MemoryType, int] = defaultdict(int)
        for mem_type in self.supported_mem_types:
            for event in mem_type_access_map[mem_type]:
                state_map[mem_type] += event.bytes_operation
                yield MemStateEvent(core_id, event.tick, state_map[mem_type], mem_type)

    def process(self) -> None:
        active_cores = IAnalyzer.get_active_cores()
        batch: List[MemStateEvent] = []
        for core_id in active_cores:
            mem_type_access_map = {
                mem_type: self.get_memory_events(self.iter_accesses(core_id, mem_type))
                for mem_type in self.supported_mem_types
            }
            batch.extend(self.get_memory_states(core_id, mem_type_access_map))
            if len(batch) >= self.BATCH_SIZE:
                self.store_UB_states(batch)
                batch.clear()
        if batch:
            self.store_UB_states(batch)
