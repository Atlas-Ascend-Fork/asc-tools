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

import abc
import sqlite3
from dataclasses import dataclass
from typing import Dict, List, Set, Tuple, Optional, Any

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer
from npu_emulate.src.common.biprof_constants import BYTES_IN_GB
from npu_emulate.src.common.extend_params import ExtendParams


@dataclass
class DMAParamsInfo(abc.ABC):
    pass


@dataclass
class DMAInstrInfo:
    params_info: Optional[DMAParamsInfo]
    processed_bytes: Optional[int]


class DMAMovAnalyzerBase(IAnalyzer):
    def __init__(self) -> None:
        self.instr_params: Dict[str, Dict[str, Tuple]] = {}
        self.used_addrs: Set[int] = set()

    @staticmethod
    def get_instr_exec_time(exec_ticks: int, aicore_freq_hz: float) -> Any:
        return (1 / aicore_freq_hz) * exec_ticks

    @staticmethod
    def convert_raw_to_gbs(raw_metric, exec_time) -> Any:
        return raw_metric / exec_time / BYTES_IN_GB

    def get_mte_instrs(self) -> List[sqlite3.Row]:
        instr_list = str(tuple(self.instr_params.keys())).replace(",)", ")")
        executed_instrs = database_handler.execute(
            """
        SELECT ExtendParams, ExecutedInstructions.ExecInstrId,
               ExecInstrName, ExecInstrTickStart, ExecInstrTickEnd
        FROM ExecutedInstructions
        WHERE ExecInstrName IN {}
        ORDER BY ExecutedInstructions.ExecInstrId
        """.format(instr_list)
        )
        result: List[sqlite3.Row] = executed_instrs.fetchall()
        return result

    @staticmethod
    def save_dma_params_info(instr_id: int, params_info) -> None:
        raise NotImplementedError

    @staticmethod
    def save_dma_processed_bytes(
        instr_id: int, processed_bytes: int, memory_traffic: float, cache_hit: bool
    ) -> None:
        database_handler.execute(
            """
        INSERT INTO DmaMovProcessedBytes
        (ExecInstrId, ProcessedBytes, MemoryTraffic, CacheHit)
        VALUES
        (?, ?, ?, ?)
        """,
            (instr_id, processed_bytes, memory_traffic, cache_hit),
        )

    def process_instruction(self, instr: sqlite3.Row) -> Optional[DMAInstrInfo]:
        raise NotImplementedError

    def process(self) -> None:
        mte_instrs = self.get_mte_instrs()
        aicore_freq_hz = database_handler.get_frequency_ghz() * 10**9
        for instr in mte_instrs:
            dma_info = self.process_instruction(instr)
            if dma_info is None:
                continue
            if dma_info.params_info is not None:
                self.save_dma_params_info(instr["ExecInstrId"], dma_info.params_info)
            if dma_info.processed_bytes is not None:
                exec_time = self.get_instr_exec_time(
                    instr["ExecInstrTickEnd"] - instr["ExecInstrTickStart"],
                    aicore_freq_hz,
                )
                ep = ExtendParams.parse(instr["ExtendParams"])
                mem_addr = ep.src_addr if ep.src_addr is not None else ep.dst_addr
                if mem_addr is None:
                    continue
                self.save_dma_processed_bytes(
                    instr["ExecInstrId"],
                    dma_info.processed_bytes,
                    self.convert_raw_to_gbs(dma_info.processed_bytes, exec_time),
                    mem_addr in self.used_addrs,
                )
                self.used_addrs.add(mem_addr)
