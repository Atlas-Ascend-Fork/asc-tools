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

from abc import ABC
import json
import os
import base64
import io
import gzip
import sqlite3
import logging as log
import traceback
from collections import defaultdict, OrderedDict
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple, Any, Union

from npu_emulate.src.common.biprof_enum import (
    CoreType,
    MemoryType,
    AnalysisType,
    ArchName,
)
from npu_emulate.src.common.biprof_constants import (
    MEM_UTIL_HEIGHT,
    MEM_UTIL_WIDTH,
    memory_size_map,
    SUPPORTED_MEM_TYPES,
    V100_SIMD_STALL_REASONS,
)
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.common.utils import (
    asdict_nocopy,
    is_in_pyinstaller_env,
    get_pyinstaller_path,
)
from npu_emulate.src.common.stage_reporter import StageReporter


ArchDiagramDescs: Dict[str, Tuple[int, str]] = {
    "aicore_memory_sim_view_bandwidth_per_operator": (0, "Bandwidth (per operator)"),
    "aicore_memory_sim_view_number_of_requests": (2, "Number of requests"),
    "aicore_memory_sim_view_bandwidth_per_request": (1, "Bandwidth (per request)"),
}


MetricsDict = {
    "execount": {"name": "Execution Count (instructions)", "mode": "sum"},
    "exetime": {"name": "Execution Time (clocks)", "mode": "sum"},
    "ubbankconflicts": {"name": "Bank Conflicts", "mode": "sum"},
    "crossinstrbankconflicts": {
        "name": "Cross Instruction Bank Conflicts",
        "mode": "sum",
    },
    "vecutil": {"name": "Vector Utilization (%)", "mode": "avg", "type": "bar"},
    "processed_bytes": {"name": "Processed Bytes", "mode": "avg"},
    "memory_traffic": {"name": "Memory Traffic (Gb/s)", "mode": "avg"},
    "gp_registers": {
        "name": "GPR Count",
        "mode": "max",
        "tooltip": "General-Purpose Registers",
    },
    "shared_registers": {
        "name": "SR Count",
        "mode": "max",
        "tooltip": "Shared Registers",
    },
    "cache_line_count": {"name": "Cache Line Count", "mode": "avg"},
    "cache_line_utilization": {
        "name": "Cache Line Utilization (%)",
        "mode": "avg",
        "type": "bar",
    },
    "mem_access_stride": {"name": "Stride (elements)", "mode": "avg"},
    "branch_efficiency": {
        "name": "Branch Efficiency (%)",
        "mode": "avg",
        "type": "bar",
    },
    "warp_occupancy": {"name": "Warp Occupancy (%)", "mode": "avg", "type": "bar"},
    "ipc": {
        "name": "IPC",
        "mode": "avg",
        "type": "multibar",
        "multibar_metric_names": {
            "scalar": "Scalar",
            "exec": "Execution",
            "ldst": "Load/Store",
            "branch": "Branch",
        },
        "max_bar_length": 0,
    },
    "samples": {
        "name": "Stall Samples (by stall reasons)",
        "mode": "sum",
        "type": "multibar",
        "multibar_metric_names": {
            "active": "Scalar Active",
            "active_pipe": "Pipe Active",
            "memory_stall": "Memory Stall",
            "execution_stall": "Execution Stall",
            "sync_stall": "Synchronization Stall",
            "queue_full_stall": "Queue Full Stall",
            "instr_fetch_stall": "Instruction Fetch Stall",
            "stall": "Other Stall",
        },
        "max_bar_length": 0,
    },
    "simd_samples": {
        "name": "SIMD Stall Reasons",
        "mode": "sum",
        "type": "multibar",
        "multibar_metric_names": {
            stall_reason: stall_reason for stall_reason in V100_SIMD_STALL_REASONS
        },
        "max_bar_length": 0,
    },
}

InstrColorScheme = {
    "VECTOR": "hsl(230, 100%, 95%)",
    "MTE": "hsl(290, 100%, 95%)",
}

HistogramColorScheme = {
    "AIV0": "#5281d5",
    "AIV1": "#6d9aea",
    "AIC": "#ff8d44",
}


@dataclass
class AsmInstruction:
    fileid: str
    linenum: int
    pc: int
    name: str
    params: str
    annotations: str
    execount: int
    exetime: int
    ubbankconflicts: int
    crossinstrbankconflicts: int
    vecutil: float
    pipe: str
    color_hotness: float
    processed_bytes: int
    memory_traffic: float
    gp_registers: int
    shared_registers: int
    cache_line_count: int
    cache_line_utilization: int
    mem_access_stride: int
    branch_efficiency: int
    warp_occupancy: int
    highlight_color: str = ""
    file_line_code: str = ""
    is_hint: bool = False
    ipc: Any = ""
    samples: Any = ""
    simd_samples: Any = ""


@dataclass
class SourceLineMetrics:
    fileid: int
    linenum: int
    execount: int
    exetime: int
    ubbankconflicts: int
    crossinstrbankconflicts: int
    vecutil: float
    color_hotness: float
    processed_bytes: int
    memory_traffic: float
    gp_registers: int
    shared_registers: int
    cache_line_count: int
    cache_line_utilization: int
    mem_access_stride: int
    branch_efficiency: int
    warp_occupancy: int
    is_hint: bool = False
    ipc: Any = ""
    samples: Any = ""
    simd_samples: Any = ""


@dataclass
class SourceFile:
    fileid: int
    filepath: str


@dataclass
class KernelSummary:
    name: str
    instr_num: str
    instrs_exec_num: int
    aggregated_instrs_time: int
    exec_time_ticks: int
    exec_time_ns: float
    start_pc: int
    arch: str
    frequency_ghz: float
    ipc: Optional[float]
    sampling_throughput: Optional[float]
    sampling_lru_cache_throughput: Optional[float]
    total_samples: Optional[int]
    active: Optional[int]
    pipe_active: Optional[int]
    queue_full_stalls: Optional[int]
    memory_stalls: Optional[int]
    execution_stalls: Optional[int]
    sync_stalls: Optional[int]
    instr_fetch_stalls: Optional[int]
    other_stall: Optional[int]
    ai_core_active: Optional[int]


@dataclass
class CallEvent:
    child_function_id: int
    call_line_id: int


@dataclass
class CallableFunction:
    function_name: str
    self_exe_count: int
    self_exe_time: int
    total_exe_count: int
    total_exe_time: int
    color_hotness: float


@dataclass
class CallGraphNode:
    id: int
    lvl: int
    name: str
    selfexecount: int
    selfexetime: int
    totalexecount: int
    totalexetime: int
    color_hotness: float
    children: list
    active: bool = False


@dataclass
class Hint(ABC):
    pass


@dataclass
class InstructionHint(Hint):
    pc: int
    fileid: int
    linenum: int
    text: str


@dataclass
class SourceLineHint(Hint):
    fileid: int
    linenum: int
    text: str


@dataclass
class KernelHint(Hint):
    text: str


class DatabaseManager:
    @staticmethod
    def get_instr_hints() -> Dict[int, List[InstructionHint]]:
        msg = """
        SELECT
        PC, FileId, LineNum, HintMsgText
        FROM InstructionHintsView
        """
        hints = database_handler.execute(msg).fetchall()
        res = defaultdict(list)
        for hint in hints:
            res[int(hint["PC"])].append(
                InstructionHint(
                    hint["PC"], hint["FileId"], hint["LineNum"], hint["HintMsgText"]
                )
            )
        return res

    @staticmethod
    def get_sourceline_hints() -> Dict[int, List[SourceLineHint]]:
        msg = """
        SELECT
        FileId, LineNum, HintMsgText
        FROM SourceLineHintsView
        """
        hints = database_handler.execute(msg).fetchall()
        res = defaultdict(list)
        for hint in hints:
            res[int(hint["LineNum"])].append(
                SourceLineHint(hint["FileId"], hint["LineNum"], hint["HintMsgText"])
            )
        return res

    @staticmethod
    def get_kernel_hints() -> List[KernelHint]:
        msg = """
        SELECT
        HintMsgText
        FROM KernelHintsView
        """
        hints = database_handler.execute(msg).fetchall()
        res: List[KernelHint] = []
        for hint in hints:
            res.append(KernelHint(hint["HintMsgText"]))
        return res

    @staticmethod
    def get_active_core_list() -> List[int]:
        return BackendDBManager.get_active_cores()

    @staticmethod
    def get_files_table() -> List[SourceFile]:
        msg = """SELECT SourceFiles.SourceFileId AS Id,
                        SourceFiles.SourceFilePath AS Path
                 FROM SourceFiles"""
        files = database_handler.execute(msg).fetchall()

        res: List[SourceFile] = []

        for file in files:
            id = file["Id"]
            path = file["Path"]
            res.append(SourceFile(id, path))

        return res

    @staticmethod
    def get_source_lines_info() -> List[SourceLineMetrics]:
        msg = """SELECT SourceLineMetrics.FileId AS FileId,
                       SourceLineMetrics.LineNum AS LineNum,
                       COALESCE(SourceLineMetrics.ExecutionCount, "")
                                                                AS ExeCount,
                       COALESCE(SourceLineMetrics.TickCount, "") AS ExeTime,
                       COALESCE(SourceLineMetrics.ProcessedBytes, "")
                                                            AS ProcessedBytes,
                       COALESCE(SourceLineMetrics.MemoryTraffic, "")
                                                            AS MemoryTraffic,
                       COALESCE(ROUND(SourceLineMetrics.CacheLineCount, 2), "")
                                                    AS CacheLineCount,
                       COALESCE(ROUND(
                                SourceLineMetrics.CacheLineUtilization, 2), "")
                                                    AS CacheLineUtilization,
                       COALESCE(ROUND(
                                SourceLineMetrics.MemAccessStride, 2), "")
                                                    AS MemAccessStride,
                       COALESCE(ROUND(SourceLineMetrics.UBBankConflicts), "")
                                                    AS UBBankConflicts,
                       COALESCE(ROUND(
                           SourceLineMetrics.UBCrossInstructionBankConflicts),
                           "") AS CrossInstructionBankConflicts,
                       COALESCE(ROUND(SourceLineMetrics.VectorUtilization, 2),
                                                    "") AS VectorUtil,
                       SourceLineMetrics.RelTime AS ColorHotness,
                       COALESCE(ROUND(SourceLineMetrics.LiveGPRegisters), "")
                                                    AS LiveGPRegisters,
                       COALESCE(ROUND(
                            SourceLineMetrics.LiveSharedRegisters), "")
                                                    AS LiveSharedRegisters,
                       COALESCE(ROUND(
                            SourceLineMetrics.BranchEfficiency), "")
                                                    AS BranchEfficiency,
                       COALESCE(ROUND(
                            SourceLineMetrics.WarpOccupancy * 100), "")
                                                    AS WarpOccupancy,
                       0 AS ScalarIPC,
                       0 AS ExecIPC,
                       0 AS LdStIPC,
                       0 AS BranchIPC,
                       COALESCE(SourceLineMetrics.Active, 0) AS Active,
                       COALESCE(SourceLineMetrics.PipeActive, 0)
                       AS PipeActive,
                       COALESCE(SourceLineMetrics.EmptyTicks, 0)
                       AS EmptyTicks,
                       COALESCE(SourceLineMetrics.MemoryStall, 0)
                       AS MemoryStall,
                       COALESCE(SourceLineMetrics.ExecutionStall, 0)
                       AS ExecutionStall,
                       COALESCE(SourceLineMetrics.QueueFullStall, 0)
                       AS QueueFullStall,
                       COALESCE(SourceLineMetrics.SyncStall, 0)
                       AS SyncStall,
                       COALESCE(SourceLineMetrics.InstrFetchStall, 0)
                       AS InstrFetchStall,
                       COALESCE(SourceLineMetrics.Stall, 0) AS Stall,
                       COALESCE(SourceLineMetrics.SIMDStallsJson, "")
                       AS SIMDStallsJson
                 FROM SourceLineMetrics
                 """
        lines = database_handler.execute(msg).fetchall()
        if not lines:
            raise ValueError("Source lines info not found.")
        res = [
            SourceLineMetrics(
                line["FileId"],
                line["LineNum"],
                line["ExeCount"],
                line["ExeTime"],
                line["UBBankConflicts"],
                line["CrossInstructionBankConflicts"],
                line["VectorUtil"],
                line["ColorHotness"],
                line["ProcessedBytes"],
                line["MemoryTraffic"],
                line["LiveGPRegisters"],
                line["LiveSharedRegisters"],
                line["CacheLineCount"],
                line["CacheLineUtilization"],
                line["MemAccessStride"],
                line["BranchEfficiency"],
                line["WarpOccupancy"],
                ipc="",
                samples={
                    "active": line["Active"],
                    "active_pipe": line["PipeActive"],
                    "memory_stall": line["MemoryStall"],
                    "execution_stall": line["ExecutionStall"],
                    "sync_stall": line["SyncStall"],
                    "queue_full_stall": line["QueueFullStall"],
                    "instr_fetch_stall": line["InstrFetchStall"],
                    "stall": line["Stall"] + line["EmptyTicks"],
                }
                if biprof_ctx.analysis_type == AnalysisType.SAMPLING
                else "",
                simd_samples={
                    stall_reason: 0 for stall_reason in V100_SIMD_STALL_REASONS
                }
                | json.loads(line["SIMDStallsJson"])
                if biprof_ctx.simd_sampling and line["SIMDStallsJson"]
                else "",
            )
            for line in lines
            if line["FileId"] is not None and line["LineNum"] is not None
        ]
        if not res:
            log.warning(
                "No source files info found. Please add -g compilation flag for ccec."
            )
        return res

    @classmethod
    def get_assembly_info(cls) -> List[AsmInstruction]:
        msg = """SELECT AsmMetrics.PC as PC,
                        AsmMetrics.InstrName as Name,
                        AsmMetrics.FileId AS FileId,
                        AsmMetrics.LineNum AS LineNum,
                        COALESCE(AsmMetrics.ExecutionCount, "") AS ExeCount,
                        COALESCE(AsmMetrics.TickCount, "") AS ExeTime,
                        COALESCE(ROUND(AsmMetrics.UBBankConflicts), "")
                                                          AS UBBankConflicts,
                        COALESCE(
                            ROUND(AsmMetrics.UBCrossInstructionBankConflicts),
                                  "") AS CrossInstructionBankConflicts,
                        COALESCE(AsmMetrics.ProcessedBytes, "")
                                                          AS ProcessedBytes,
                        COALESCE(AsmMetrics.MemoryTraffic, "")
                                                          AS MemoryTraffic,
                        COALESCE(ROUND(AsmMetrics.CacheLineCount, 2), "")
                                                    AS CacheLineCount,
                        COALESCE(ROUND(AsmMetrics.CacheLineUtilization, 2), "")
                                                    AS CacheLineUtilization,
                        COALESCE(ROUND(AsmMetrics.MemAccessStride, 2), "")
                                                    AS MemAccessStride,
                        COALESCE(ROUND(AsmMetrics.VectorUtilization, 2), "")
                                                                 AS VectorUtil,
                        AsmMetrics.Pipe AS Pipe,
                        AsmMetrics.RelTime AS ColorHotness,
                        COALESCE(AsmMetrics.LiveGPRegisters, "")
                            AS LiveGPRegisters,
                        COALESCE(AsmMetrics.LiveSharedRegisters, "")
                            AS LiveSharedRegisters,
                        COALESCE(AsmMetrics.VfSimtTidX, "") AS VfSimtTidX,
                        COALESCE(AsmMetrics.VfSimtTidY, "") AS VfSimtTidY,
                        COALESCE(AsmMetrics.VfSimtTidZ, "") AS VfSimtTidZ,
                        COALESCE(AsmMetrics.VfSimtRegNum, "")
                            AS VfSimtRegNum,
                        COALESCE(
                            ROUND(AsmMetrics.BranchEfficiency), "")
                                AS BranchEfficiency,
                        COALESCE(ROUND(AsmMetrics.WarpOccupancy * 100), "")
                            AS WarpOccupancy,
                        AsmMetrics.ScalarIPC AS ScalarIPC,
                        AsmMetrics.ExecIPC AS ExecIPC,
                        AsmMetrics.LdStIPC AS LdStIPC,
                        AsmMetrics.BranchIPC AS BranchIPC,
                        COALESCE(AsmMetrics.Active, 0) AS Active,
                        COALESCE(AsmMetrics.PipeActive, 0) AS PipeActive,
                        COALESCE(AsmMetrics.MemoryStall, 0) AS MemoryStall,
                        COALESCE(AsmMetrics.EmptyTicks, 0) AS EmptyTicks,
                        COALESCE(AsmMetrics.ExecutionStall, 0)
                        AS ExecutionStall,
                        COALESCE(AsmMetrics.QueueFullStall, 0)
                        AS QueueFullStall,
                        COALESCE(AsmMetrics.SyncStall, 0)
                        AS SyncStall,
                        COALESCE(AsmMetrics.InstrFetchStall, 0)
                        AS InstrFetchStall,
                        COALESCE(AsmMetrics.Stall, 0) AS Stall,
                        COALESCE(AsmMetrics.SIMDStallsJson, "")
                        AS SIMDStallsJson
                 FROM AsmMetrics
                 GROUP BY AsmMetrics.PC
                """
        lines = database_handler.execute(msg).fetchall()

        msg = """
        SELECT SourceInstrAddr, BBName
        FROM BasicBlocks
        """
        bbs = OrderedDict(
            [
                (row["SourceInstrAddr"], row["BBName"])
                for row in database_handler.execute(msg)
            ]
        )

        msg = """
        SELECT StartBlockId, EndBlockId
        FROM BasicBlocksRelations
        """
        bb_relations = [
            (row["StartBlockId"], row["EndBlockId"])
            for row in database_handler.execute(msg)
        ]

        msg = """
        SELECT SourceInstrAddr, FunctionName
        FROM Functions
        """
        funcs = OrderedDict(
            [
                (row["SourceInstrAddr"], row["FunctionName"])
                for row in database_handler.execute(msg)
            ]
        )

        if not lines:
            raise TypeError("Assembly lines info not found.")

        @staticmethod
        def _has_all_ipc_fields(line: Dict) -> bool:
            return (
                line["ScalarIPC"] is not None
                and line["ExecIPC"] is not None
                and line["LdStIPC"] is not None
                and line["BranchIPC"] is not None
            )

        def parse_name(name_with_params: str) -> Tuple[str, str]:
            name, _, params = name_with_params.partition(" ")
            return name, params

        res = []

        columns_list = (
            "ExeCount",
            "ExeTime",
            "UBBankConflicts",
            "CrossInstructionBankConflicts",
            "VectorUtil",
            "Pipe",
            "ColorHotness",
            "ProcessedBytes",
            "MemoryTraffic",
            "LiveGPRegisters",
            "LiveSharedRegisters",
            "CacheLineCount",
            "CacheLineUtilization",
            "MemAccessStride",
            "BranchEfficiency",
            "WarpOccupancy",
        )

        last_bb_idx = -1

        for line in lines:
            if line["Name"] is None:
                continue
            name, params = parse_name(line["Name"])
            annotations = ""
            if line["VfSimtRegNum"]:
                # Annotate VF_SIMT
                annotations += (
                    f"<<<{line['VfSimtTidX']}, {line['VfSimtTidY']}, "
                    f"{line['VfSimtTidZ']}>>>, "
                    f"available GPRs: {line['VfSimtRegNum']}"
                )

            # Basic Block insertion
            if line["PC"] in funcs.keys():
                res.append(
                    AsmInstruction(
                        "0",
                        0,
                        line["PC"],
                        f"Function {funcs[line['PC']]}:",
                        "",
                        "",
                        *([""] * len(columns_list)),  # type:ignore
                    )
                )
            if line["PC"] in bbs.keys():
                cur_bb_idx = sorted(bbs.keys()).index(line["PC"])

                # Add not executed basic blocks
                if last_bb_idx + 1 < cur_bb_idx:
                    for bb_idx in range(last_bb_idx + 1, cur_bb_idx):
                        res.append(
                            cls._add_basic_block(
                                sorted(bbs.keys())[bb_idx],
                                bbs,
                                bb_relations,
                                columns_list,
                            )
                        )
                last_bb_idx = cur_bb_idx

                res.append(
                    cls._add_basic_block(line["PC"], bbs, bb_relations, columns_list)
                )
            highlighting_color = cls.get_instr_highlighting_color(line["Pipe"])

            ipc: Any = ""
            if biprof_ctx.scalar_ipc or biprof_ctx.vec_ipc:
                if _has_all_ipc_fields(line):
                    ipc = {
                        "scalar": line["ScalarIPC"],
                        "exec": line["ExecIPC"],
                        "ldst": line["LdStIPC"],
                        "branch": line["BranchIPC"],
                    }

            samples = (
                {
                    "active": line["Active"],
                    "active_pipe": line["PipeActive"],
                    "memory_stall": line["MemoryStall"],
                    "execution_stall": line["ExecutionStall"],
                    "sync_stall": line["SyncStall"],
                    "queue_full_stall": line["QueueFullStall"],
                    "instr_fetch_stall": line["InstrFetchStall"],
                    "stall": line["Stall"] + line["EmptyTicks"],
                }
                if biprof_ctx.analysis_type == AnalysisType.SAMPLING
                else ""
            )

            simd_samples = (
                {stall_reason: 0 for stall_reason in V100_SIMD_STALL_REASONS}
                | json.loads(line["SIMDStallsJson"])
                if biprof_ctx.simd_sampling and line["SIMDStallsJson"]
                else ""
            )

            instr = AsmInstruction(
                line["FileId"],
                line["LineNum"],
                line["PC"],
                name,
                params,
                annotations,
                *(line[p] for p in columns_list),
            )
            instr.highlight_color = highlighting_color
            instr.ipc = ipc
            instr.samples = samples
            instr.simd_samples = simd_samples
            res.append(instr)
        return res

    @staticmethod
    def get_instr_highlighting_color(pipe) -> str:
        if pipe is None:
            return ""
        if pipe.startswith("RV"):
            return InstrColorScheme["VECTOR"]
        if pipe.startswith("MTE"):
            return InstrColorScheme["MTE"]
        return ""

    @staticmethod
    def get_memory_utilization_states(core_id: int, mem_type: int) -> List[sqlite3.Row]:
        cell_size = (
            memory_size_map[biprof_ctx.arch][MemoryType(mem_type)]
            // MEM_UTIL_HEIGHT
            // MEM_UTIL_WIDTH
        )
        states = database_handler.execute(
            """
            SELECT (MemoryRWAccesses.AccessedAddress / {}) AS CellId,
                   COUNT(MemoryRWAccesses.AccessMode) AS AccessCount
            FROM MemoryRWAccesses
            WHERE CoreId=:core_id AND
                  MemoryType=:memory_type
            GROUP BY CellId
        """.format(cell_size),
            {"core_id": core_id, "memory_type": mem_type},
        )
        data = states.fetchall()
        return data

    @staticmethod
    def _add_basic_block(
        addr: int,
        bbs: OrderedDict,
        bb_relations: List[Tuple[int, int]],
        columns_list: Tuple[str, ...],
    ) -> AsmInstruction:
        bb = bbs[addr]
        preds = sorted(
            [
                int(bbs[beg][len("Basic Block ") :])
                for beg, _ in filter(lambda t: t[1] == addr, bb_relations)
            ]
        )
        succs = sorted(
            [
                int(bbs[end][len("Basic Block ") :])
                for _, end in filter(lambda t: t[0] == addr, bb_relations)
            ]
        )
        bb_annotations = (
            f"pred: [{', '.join((str(x) for x in preds))}], "
            f"succ: [{', '.join((str(x) for x in succs))}]"
        )
        return AsmInstruction(
            "0", 0, addr, f"{bb}:", "", bb_annotations, *([""] * len(columns_list))
        )  # type:ignore

    @staticmethod
    def get_kernel_summary() -> KernelSummary:
        msg = """SELECT KernelInfo.KernelInfoAttr AS Name,
                        KernelInfo.KernelInfoVal AS Val
                 FROM KernelInfo"""
        kernel_info_data = database_handler.execute(msg).fetchall()
        if not kernel_info_data:
            raise TypeError("Kernel summary info not found.")

        ki = {i["Name"]: i["Val"] for i in kernel_info_data}

        return KernelSummary(
            ki["name"],
            ki["instr_num"],
            ki["instrs_exec_num"],
            ki["instrs_time_sum"],
            ki["exec_time_ticks"],
            ki["exec_time_ns"],
            ki["start_pc"],
            ki["arch"],
            ki["frequency_ghz"],
            ki.get("scalar_ipc", None),
            ki.get("sampling_throughput", None),
            ki.get("sampling_lru_cache_throughput", None),
            ki.get("total_samples", None),
            ki.get("active", None),
            ki.get("pipe_active", None),
            ki.get("queue_full_stalls", None),
            ki.get("memory_stalls", None),
            ki.get("execution_stalls", None),
            ki.get("sync_stalls", None),
            ki.get("instr_fetch_stalls", None),
            ki.get("other_stall", None),
            ki.get("ai_core_active", None),
        )

    @staticmethod
    def get_instructions_name_hist_count(max_ln: int) -> List[Dict[str, Any]]:
        msg = """SELECT InstrNameHistCount.Name AS Name,
                        InstrNameHistCount.Count AS Count
                 FROM InstrNameHistCount"""
        names = database_handler.execute(msg).fetchall()
        res = [{"x": itype["Name"], "count": itype["Count"]} for itype in names]
        res.sort(key=lambda x: x["count"], reverse=True)
        res = res[:max_ln]
        return res

    @staticmethod
    def get_instructions_name_hist_clocks(max_ln: int) -> List[Dict[str, Any]]:
        msg = """SELECT InstrNameHistClocks.Name AS Name,
                        InstrNameHistClocks.TickCount AS TickCount
                 FROM InstrNameHistClocks"""
        names = database_handler.execute(msg).fetchall()
        res = [{"x": itype["Name"], "count": itype["TickCount"]} for itype in names]
        res.sort(key=lambda x: x["count"], reverse=True)
        res = res[:max_ln]
        return res

    @staticmethod
    def get_basic_block_exe_count_diagram() -> List[Dict[str, Any]]:
        msg = """SELECT BasicBlockCountDiagram.Name AS Name,
                        BasicBlockCountDiagram.ExeCount AS ExeCount
                 FROM BasicBlockCountDiagram"""
        bbs = database_handler.execute(msg).fetchall()
        res = [{"x": bb["Name"], "count": bb["ExeCount"]} for bb in bbs]

        res.sort(key=lambda x: int(x["x"][len("Basic Block ") :]))
        return res

    @staticmethod
    def get_instructions_type_hist_count() -> List[Dict[str, Any]]:
        msg = """SELECT InstrTypeHistCount.Type AS Type,
                        InstrTypeHistCount.Count AS Count
                 FROM   InstrTypeHistCount"""
        types = database_handler.execute(msg).fetchall()
        msg = """SELECT InstrTypes.InstrTypeName AS Type
                 FROM   InstrTypes"""
        all_types = database_handler.execute(msg).fetchall()
        type_template = {it["Type"]: 0 for it in all_types}
        for i in types:
            key, val = dict(i)["Type"], dict(i)["Count"]
            if key in type_template:
                type_template[key] = val
        return [{"x": key, "count": val} for key, val in type_template.items()]

    @staticmethod
    def get_instructions_type_hist_clocks() -> List[Dict[str, Any]]:
        msg = """SELECT InstrTypeHistClocks.Type AS Type,
                        InstrTypeHistClocks.TickCount AS TickCount
                 FROM   InstrTypeHistClocks"""
        types = database_handler.execute(msg).fetchall()

        if len(types) == 0:
            return []

        msg = """SELECT InstrTypes.InstrTypeName AS Type
                 FROM   InstrTypes"""
        all_types = database_handler.execute(msg).fetchall()
        type_template = {it["Type"]: 0 for it in all_types}
        for i in types:
            key, val = dict(i)["Type"], dict(i)["TickCount"]
            if key in type_template:
                type_template[key] = val
        return [{"x": key, "count": val} for key, val in type_template.items()]

    @staticmethod
    def get_pipes_utilization_hist() -> List[Dict[str, Any]]:
        msg = """SELECT PipeUtilizationHist.PipeName AS PipeName,
                        PipeUtilizationHist.CoreName AS CoreName,
                        PipeUtilizationHist.Utilization AS Utilization
                 FROM PipeUtilizationHist"""
        names = database_handler.execute(msg).fetchall()
        res: Dict[str, Dict[str, Any]] = defaultdict(dict)
        for rec in names:
            pipe = rec["PipeName"]
            core = rec["CoreName"]
            val = rec["Utilization"]
            res[pipe][core] = val
        return [{"x": key, **val} for key, val in res.items()]

    @staticmethod
    def get_pipe_utilization_series() -> List[Dict[str, Any]]:
        msg = """SELECT CoreTypes.CoreTypeName AS name
                 FROM CoreTypes"""
        names = database_handler.execute(msg).fetchall()
        return [
            {
                "yKey": key["name"],
                "Name": key["name"],
                "fill": HistogramColorScheme[key["name"]],
            }
            for key in names
        ]

    @staticmethod
    def get_aicore_occupancy_hist() -> List[Dict[str, Any]]:
        items = database_handler.execute(
            """
            SELECT CoreId, CoreTypeName, TickStart, TickEnd
            FROM AiCoreOccupancy
            LEFT JOIN CoreTypes
            ON AiCoreOccupancy.CoreTypeId = CoreTypes.CoreTypeId
            """
        ).fetchall()

        start = min(items, key=lambda item: item["TickStart"])["TickStart"]
        end = max(items, key=lambda item: item["TickEnd"])["TickEnd"]
        total_dur = end - start
        res: Dict[str, Dict[str, Any]] = defaultdict(dict)
        for item in items:
            utilization = (item["TickEnd"] - item["TickStart"]) / total_dur * 100.0
            res[f"CORE {item['CoreId']}"][item["CoreTypeName"]] = utilization
        return [{"x": key, **val} for key, val in res.items()]

    @staticmethod
    def get_aicore_occupancy_series() -> List[Dict[str, Any]]:
        msg = """SELECT CoreTypes.CoreTypeName AS name
                 FROM CoreTypes"""
        names = database_handler.execute(msg).fetchall()
        return [
            {
                "yKey": key["name"],
                "Name": key["name"],
                "fill": HistogramColorScheme[key["name"]],
            }
            for key in names
        ]

    @staticmethod
    def get_scalar_ipc_dynamic_diagram(
        core_id: int, core_type_id: int
    ) -> List[Dict[str, Any]]:
        rows = database_handler.execute(
            """
            SELECT
                ScalarIpcDynamic.Tick AS Tick,
                ScalarIpcDynamic.IPC AS Value
                 FROM ScalarIpcDynamic
            WHERE
                ScalarIpcDynamic.CoreId = :core_id
                AND ScalarIpcDynamic.CoreTypeId = :core_type_id
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchall()
        res = [{"x": (i + 1), "count": row["Value"]} for i, row in enumerate(rows)]
        return res

    @staticmethod
    def get_active_types() -> List[str]:
        msg = """SELECT ActiveInstrTypes.Name AS Name
                 FROM   ActiveInstrTypes"""
        types = database_handler.execute(msg).fetchall()
        return [itype["Name"] for itype in types]

    @staticmethod
    def get_call_event_map() -> Dict[str, List[CallEvent]]:
        msg = """SELECT CallGraph.CallFunctionId AS id,
                        CallGraph.ChildFunctionId AS child_id,
                        CallGraph.CallSiteLine AS line_id
                  FROM CallGraph"""
        call_events = database_handler.execute(msg).fetchall()
        res: Dict[str, List[CallEvent]] = defaultdict(list)
        for event in call_events:
            res[event["id"]].append(CallEvent(event["child_id"], event["line_id"]))
        return res

    @staticmethod
    def get_call_functions() -> Dict[str, CallableFunction]:
        msg = """SELECT MAX(CallGraphMetrics.TotalTickCount) as MaxTick
                 FROM CallGraphMetrics"""
        max_instr_time = database_handler.execute(msg).fetchone()["MaxTick"]

        msg = """SELECT CallFunctions.CallFunctionId AS id,
                        CallFunctions.FunctionName AS name,
                        CallGraphMetrics.SelfExecutionCount AS self_count,
                        CallGraphMetrics.SelfTickCount AS self_time,
                        CallGraphMetrics.TotalExecutionCount AS total_count,
                        CallGraphMetrics.TotalTickCount AS total_time,
                        (CAST(CallGraphMetrics.TotalTickCount AS float) /
                                                    :time) AS color_hotness
                 FROM CallFunctions
                 LEFT JOIN CallGraphMetrics
                 ON CallFunctions.CallFunctionId =
                            CallGraphMetrics.CallFunctionId"""
        functions = database_handler.execute(msg, {"time": max_instr_time}).fetchall()

        return {
            f["id"]: CallableFunction(
                f["name"],
                f["self_count"],
                f["self_time"],
                f["total_count"],
                f["total_time"],
                f["color_hotness"],
            )
            for f in functions
        }


class CallGraphGenerator:
    def __init__(self, kernel_name) -> None:
        self.kernel_name = kernel_name
        self.events = DatabaseManager.get_call_event_map()
        self.functions = DatabaseManager.get_call_functions()
        self.node_id = 0
        self.root_id = None
        root_name = kernel_name + "_root"
        for f in self.functions.keys():
            if root_name == self.functions[f].function_name:
                self.root_id = f
                break

        if not self.root_id:
            log.warning("Source-Assembly did not find root function id.")
            raise ValueError("Source-Assembly did not find root function id.")

    def get_id(self) -> int:
        id = self.node_id
        self.node_id += 1
        return id

    def create_call_graph_json(self):
        if not self.events:
            return {}
        res = self.get_callable_nodes(self.root_id, 0)
        return res

    def get_callable_nodes(self, parent_id, lvl) -> Any:
        parent_data: CallableFunction = self.functions[parent_id]
        parent_events = []
        if parent_id in self.events:
            parent_events = self.events[parent_id]
        id = self.get_id()
        child_events = []
        for event in parent_events:
            child_events.append(
                self.get_callable_nodes(event.child_function_id, lvl + 1)
            )
        child_events.sort(key=lambda x: x["totalexetime"], reverse=True)
        return asdict_nocopy(
            CallGraphNode(
                id,
                lvl,
                parent_data.function_name,
                parent_data.self_exe_count,
                parent_data.self_exe_time,
                parent_data.total_exe_count,
                parent_data.total_exe_time,
                parent_data.color_hotness * 100,
                child_events,
            )
        )


class SourceAsmHTMLGenerator:
    def __init__(self) -> None:
        self.kernel_name = ""

    def prepare_report(self, output_dir: str, kernel_name: str) -> int:
        self.kernel_name = kernel_name
        arch_diagram_data = {}
        if os.path.isdir(output_dir):
            arch_diagram_data = self.__read_arch_diagrams(output_dir)
        try:
            with StageReporter("Source-Assembly report: getting HTML template"):
                html_temp = self.__get_html_template()
            with StageReporter("Source-Assembly report: getting JSON data"):
                json_data = self.__get_json(arch_diagram_data)
            with StageReporter("Source-Assembly report: zipping JSON data"):
                bjson = self.__zip_json(json_data)
            with StageReporter("Source-Assembly report: injecting JSON data"):
                html_text = html_temp.replace("<!--DATA_PLACE-->", bjson)
            with open(f"{output_dir}/source_assembly_report.html", "w+") as output:
                with StageReporter("Source-Assembly report: writing data to file"):
                    output.write(html_text)

            ret = 0
        except TypeError as error:
            log.error(str(error))
            log.debug(traceback.format_exc())
            ret = 1
        except ValueError as error:
            log.warning("Source-Assembly report generation skipped")
            log.warning(str(error))
            ret = 0
        return ret

    @staticmethod
    def __read_arch_diagrams(diag_path: str) -> Dict[str, str]:
        res = [("", "")] * len(os.listdir(diag_path))
        for file in os.listdir(diag_path):
            if file.split(".")[0] in ArchDiagramDescs:
                path = os.path.join(diag_path, file)
                desc_info = ArchDiagramDescs.get(file[:-4], "")
                with open(path, "r") as f:
                    res[int(desc_info[0])] = (desc_info[1], f.read())
        return dict(res)

    @staticmethod
    def __get_html_template() -> str:
        if is_in_pyinstaller_env():
            base_path = get_pyinstaller_path()
        else:
            base_path = os.path.join(os.path.dirname(__file__), "..", "..")
        path_to_template = os.path.abspath(
            os.path.join(base_path, "static/sourceAssemblyTemplate.html")
        )
        with open(path_to_template, "r") as temp:
            text_temp = temp.read()
        return text_temp

    @staticmethod
    def update_assembly_with_file_lines(
        assembly_data: List[AsmInstruction], files_json
    ) -> List[AsmInstruction]:
        for instr in assembly_data:
            if instr.fileid in files_json:
                file_line_code = f"{files_json[instr.fileid]}:{instr.linenum}"
                instr.file_line_code = file_line_code
        return assembly_data

    @staticmethod
    def update_assembly(
        asm_data: List[AsmInstruction],
        instr_hints_map: Dict[int, List[InstructionHint]],
    ) -> None:
        for instr in asm_data:
            if int(instr.pc) in instr_hints_map and "Basic Block" not in instr.name:
                instr.is_hint = True

    @staticmethod
    def update_source(
        source_data: List[SourceLineMetrics],
        sourceline_hints_map: Dict[int, List[SourceLineHint]],
    ) -> None:
        for item in source_data:
            if int(item.linenum) in sourceline_hints_map:
                item.is_hint = True

    @staticmethod
    def get_color_label():
        return InstrColorScheme

    @staticmethod
    def handle_multibar_metric(source_data, asm_json_data, key: str) -> None:
        max_len_val = 0
        for lines in source_data.values():
            loc_max = max(
                [
                    sum(0 if val == "" else val for val in line[key].values())
                    for line in lines
                    if isinstance(line[key], dict)
                ],
                default=0,
            )
            max_len_val = max(max_len_val, loc_max)
        for line in asm_json_data:
            if not isinstance(line[key], dict):
                continue
            max_len_val = max(
                max_len_val, sum(0 if val == "" else val for val in line[key].values())
            )
        if isinstance(MetricsDict[key], dict):
            MetricsDict[key]["max_bar_length"] = max_len_val  # type:ignore

    def __get_json(self, arch_diagram_data) -> str:
        res = {
            "sourceasm": self.__create_source_asm_json(),
            "kernelinfo": {
                "graphics": self.__get_graphics_json(),
                "kernelsummary": self.__create_kernel_summary_json(),
            },
            "callgraph": self.__create_call_graph_json(self.kernel_name),
            "arch_diagram": arch_diagram_data,
            "mem_util": self.__create_mem_util_json(),
        }
        return json.dumps(res)

    def __create_source_asm_json(self) -> Dict[str, Any]:
        if not biprof_ctx.object_file:
            return {}
        instr_hints_map = DatabaseManager.get_instr_hints()
        sourceline_hints_map = DatabaseManager.get_sourceline_hints()
        kernel_hints = DatabaseManager.get_kernel_hints()
        files_data = DatabaseManager.get_files_table()
        files_json = self.__create_files_json(files_data)
        source_data = DatabaseManager.get_source_lines_info()
        self.update_source(source_data, sourceline_hints_map)
        assembly_data = DatabaseManager.get_assembly_info()
        self.update_assembly(assembly_data, instr_hints_map)
        if not source_data:
            self.update_assembly_with_file_lines(assembly_data, files_json)
        asm_json = self.__create_asm_json(assembly_data)
        source_json = self.__create_source_json(source_data, files_data)
        hints_json = self.__create_hint_json(
            instr_hints_map, sourceline_hints_map, kernel_hints
        )
        if biprof_ctx.analysis_type == AnalysisType.SAMPLING:
            self.handle_multibar_metric(source_json, asm_json, "samples")
        if biprof_ctx.scalar_ipc or biprof_ctx.vec_ipc:
            self.handle_multibar_metric(source_json, asm_json, "ipc")
        if biprof_ctx.simd_sampling and ArchName(biprof_ctx.arch) == ArchName.V100:
            self.handle_multibar_metric(source_json, asm_json, "simd_samples")
        return {
            "hints": hints_json,
            "files": files_json,
            "source": source_json,
            "assembly": asm_json,
            "active_types": DatabaseManager.get_active_types(),
            "active_metrics": self.__get_active_metrics(asm_json),
            "color_label": self.get_color_label(),
        }

    def __create_mem_util_json(self) -> Dict[int, Any]:
        res: Dict[int, Any] = defaultdict(list)
        if not biprof_ctx.memory_utilization:
            return res
        for core in DatabaseManager.get_active_core_list():
            util_buff = {}
            for mem_type in SUPPORTED_MEM_TYPES:
                states = DatabaseManager.get_memory_utilization_states(
                    core, mem_type.value
                )
                if not states:
                    continue
                max_access_count = max(states, key=lambda x: x["AccessCount"])[
                    "AccessCount"
                ]
                usage: float = 0.0
                data = {}
                for state in states:
                    cell_id = state["CellId"]
                    acc_count = state["AccessCount"]
                    if acc_count != 0:
                        usage += 1
                    data[cell_id] = {
                        "col": (acc_count / max_access_count) * 100.0,
                        "metrics": {"acc_count": acc_count},
                    }
                name = mem_type.name.replace("_", " ")
                total_size = memory_size_map[biprof_ctx.arch][MemoryType(mem_type)]
                cell_size = total_size // MEM_UTIL_HEIGHT // MEM_UTIL_WIDTH
                usage = usage / MEM_UTIL_HEIGHT / MEM_UTIL_WIDTH
                util_buff[mem_type] = {
                    "name": name,
                    "width": MEM_UTIL_WIDTH,
                    "height": MEM_UTIL_HEIGHT,
                    "cell_size": cell_size,
                    "total_size": total_size / 1024,
                    "usage": usage,
                    "start_addr": 0x0,
                    "metrics": {"acc_count": "Accesses Count"},
                    "data": data,
                }
            if util_buff:
                res[core].extend(self.__get_named_buff_dict(util_buff))

        return res

    @staticmethod
    def __get_named_buff_dict(util_buff) -> List[Any]:
        res: List[Dict[str, Union[str, List[Any]]]] = []
        if MemoryType.AIV0_UB in util_buff:
            res.append(
                {"name": "UB Utilization", "buff": [util_buff[MemoryType.AIV0_UB]]}
            )
        if MemoryType.AIV1_UB in util_buff:
            if not res:
                res.append(
                    {"name": "UB Utilization", "buff": [util_buff[MemoryType.AIV1_UB]]}
                )
            else:
                res[-1]["buff"] = [
                    util_buff[MemoryType.AIV0_UB],
                    util_buff[MemoryType.AIV1_UB],
                ]
        if MemoryType.AIC_L1 in util_buff:
            res.append(
                {"name": "L1 Utilization", "buff": [util_buff[MemoryType.AIC_L1]]}
            )
        if MemoryType.AIC_L0A in util_buff:
            res.append(
                {"name": "L0A/L0B Utilization", "buff": [util_buff[MemoryType.AIC_L0A]]}
            )
        if MemoryType.AIC_L0B in util_buff:
            if not res or res[-1]["name"] != "L0A/L0B Utilization":
                res.append(
                    {
                        "name": "L0A/L0B Utilization",
                        "buff": [util_buff[MemoryType.AIC_L0B]],
                    }
                )
            else:
                res[-1]["buff"] = [
                    util_buff[MemoryType.AIC_L0A],
                    util_buff[MemoryType.AIC_L0B],
                ]
        if MemoryType.AIC_L0C in util_buff:
            res.append(
                {"name": "L0C Utilization", "buff": [util_buff[MemoryType.AIC_L0C]]}
            )
        return res

    @staticmethod
    def __get_graphics_json() -> Any:
        TOP_N = 20
        instr_types_count_points = DatabaseManager.get_instructions_type_hist_count()
        instr_types_clocks_points = DatabaseManager.get_instructions_type_hist_clocks()
        instr_names_count_points = DatabaseManager.get_instructions_name_hist_count(
            TOP_N
        )
        instr_names_clocks_points = DatabaseManager.get_instructions_name_hist_clocks(
            TOP_N
        )
        bb_count_points = DatabaseManager.get_basic_block_exe_count_diagram()
        pipe_utilization_points = DatabaseManager.get_pipes_utilization_hist()
        pipe_utilization_series = DatabaseManager.get_pipe_utilization_series()
        aicore_occupancy_points = DatabaseManager.get_aicore_occupancy_hist()
        aicore_occupancy_series = DatabaseManager.get_aicore_occupancy_series()

        scalar_ipc_diagrams = []
        for core_type_id in BackendDBManager.get_active_core_types_ordered():
            scalar_ipc_points = DatabaseManager.get_scalar_ipc_dynamic_diagram(
                0, core_type_id
            )
            if not scalar_ipc_points:
                continue
            scalar_ipc_diagrams.append(
                {
                    "name": (f"Scalar IPC Dynamic ({CoreType(core_type_id).name})"),
                    "type": "diag",
                    "xlabel": "Window Number",
                    "ylabel": "Scalar IPC Value",
                    "data": scalar_ipc_points,
                    "series": [{"yKey": "count", "Name": "Scalar IPC Value"}],
                }
            )

        return [
            {
                "name": "Pipe Utilization Histogram",
                "type": "hist",
                "xlabel": "Pipe Utilization (%)",
                "ylabel": "",
                "xMin": 0,
                "xMax": 100,
                "data": pipe_utilization_points,
                "series": pipe_utilization_series,
            },
            {
                "name": "Aicore Utilization Histogram",
                "type": "hist",
                "xlabel": "Aicore Utilization (%)",
                "ylabel": "",
                "xMin": 0,
                "xMax": 100,
                "data": aicore_occupancy_points,
                "series": aicore_occupancy_series,
            },
            {
                "name": "Instruction Type Histogram (count)",
                "type": "hist",
                "xlabel": "Instruction Count",
                "ylabel": "",
                "data": instr_types_count_points,
                "series": [{"yKey": "count", "Name": "Execution Count"}],
            },
            {
                "name": "Instruction Type Histogram (clocks)",
                "type": "hist",
                "xlabel": "Instruction Clocks",
                "ylabel": "",
                "data": instr_types_clocks_points,
                "series": [{"yKey": "count", "Name": "Instruction Clocks"}],
            },
            {
                "name": "Instruction Name Histogram (count)",
                "type": "hist",
                "xlabel": "Instruction Count",
                "ylabel": "",
                "data": instr_names_count_points,
                "series": [{"yKey": "count", "Name": "Instruction Count"}],
            },
            {
                "name": "Instruction Name Histogram (clocks)",
                "type": "hist",
                "xlabel": "Instruction Clocks",
                "ylabel": "",
                "data": instr_names_clocks_points,
                "series": [{"yKey": "count", "Name": "Instruction Clocks"}],
            },
            {
                "name": "Basic Block Count Diagram",
                "type": "diag",
                "xlabel": "Basic Block ID",
                "ylabel": "Execution Count",
                "data": bb_count_points,
                "series": [{"yKey": "count", "Name": "Execution Count"}],
            },
            *scalar_ipc_diagrams,
        ]

    @staticmethod
    def __zip_json(string_json: str) -> str:
        compressed_data = io.BytesIO()
        with gzip.GzipFile(fileobj=compressed_data, mode="wb") as f:
            f.write(string_json.encode("utf-8"))
        b64_content = base64.b64encode(compressed_data.getvalue())
        return b64_content.decode("utf-8")

    @staticmethod
    def __create_kernel_summary_json() -> Any:
        kernel_sum_data = DatabaseManager.get_kernel_summary()
        return [
            {"name": "Operator Name", "hint": "", "data": kernel_sum_data.name},
            {
                "name": "Static Instructions (count)",
                "hint": "Number of instructions in the device object file",
                "data": kernel_sum_data.instr_num,
            },
            {
                "name": "Dynamic Instructions (count)",
                "hint": "Total number of instructions executed",
                "data": kernel_sum_data.instrs_exec_num,
            },
            {
                "name": "Operator Execution Time (us)",
                "hint": "Overall operator duration in us",
                "data": round(kernel_sum_data.exec_time_ns / 1000, 2),
            },
            {
                "name": "Scalar IPC Rate",
                "hint": "Instruction-per-cycle",
                "data": kernel_sum_data.ipc,
            },
            {
                "name": "Aggregated Instructions Time (clocks)",
                "hint": (
                    "Sum of clocks for all instructions executed "
                    "(instructions may be executed in parallel)"
                ),
                "data": kernel_sum_data.aggregated_instrs_time,
            },
            {"name": "Start PC", "hint": "", "data": hex(kernel_sum_data.start_pc)},
            {"name": "Architecture", "hint": "", "data": kernel_sum_data.arch},
            {
                "name": "Core Frequency (GHz)",
                "hint": "",
                "data": kernel_sum_data.frequency_ghz,
            },
            {
                "name": "AI Core Active",
                "hint": "",
                "data": kernel_sum_data.ai_core_active,
            },
            {
                "name": "Sampling Throughput without cache (Mb/s)",
                "hint": "",
                "data": kernel_sum_data.sampling_throughput,
            },
            {
                "name": "Sampling Throughput with LRU cache (Mb/s)",
                "hint": "",
                "data": kernel_sum_data.sampling_lru_cache_throughput,
            },
            {
                "name": "Total Samples",
                "hint": "",
                "data": kernel_sum_data.total_samples,
            },
            {
                "name": "Active Scalar Samples",
                "hint": "",
                "data": kernel_sum_data.active,
            },
            {
                "name": "Active Pipe Samples",
                "hint": "",
                "data": kernel_sum_data.pipe_active,
            },
            {
                "name": "Memory Stall Scalar Samples",
                "hint": "",
                "data": kernel_sum_data.memory_stalls,
            },
            {
                "name": "Execution Stall Scalar Samples",
                "hint": "",
                "data": kernel_sum_data.execution_stalls,
            },
            {
                "name": "Instruction Fetch Stall Scalar Samples",
                "hint": "",
                "data": kernel_sum_data.instr_fetch_stalls,
            },
            {
                "name": "Queue Full Stall Scalar Samples",
                "hint": "",
                "data": kernel_sum_data.queue_full_stalls,
            },
            {
                "name": "Synchronization Stall Scalar Samples",
                "hint": "",
                "data": kernel_sum_data.sync_stalls,
            },
            {
                "name": "Other Stall Samples",
                "hint": "",
                "data": kernel_sum_data.other_stall,
            },
        ]

    @staticmethod
    def __create_files_json(files_data: List[SourceFile]) -> Dict[int, str]:
        result = {}
        for file in files_data:
            if os.path.exists(file.filepath):
                result[file.fileid] = file.filepath
        return result

    @staticmethod
    def __create_asm_json(assembly_data: List[AsmInstruction]) -> List[Any]:
        return [asdict_nocopy(line) for line in assembly_data]

    @staticmethod
    def __create_hint_json(
        instr_hints_map: Dict[int, List[InstructionHint]],
        sourceline_hints_map: Dict[int, List[SourceLineHint]],
        kernel_hints: List[KernelHint],
    ) -> List[Any]:
        res = list()
        for instr_hints in instr_hints_map.values():
            res.extend([asdict_nocopy(hint) for hint in instr_hints])
        for sourceline_hints in sourceline_hints_map.values():
            res.extend([asdict_nocopy(hint) for hint in sourceline_hints])
        res.extend([asdict_nocopy(hint) for hint in kernel_hints])
        return res

    def __create_source_json(
        self,
        executed_source_lines: List[SourceLineMetrics],
        files_data: List[SourceFile],
    ) -> Dict[int, List[Dict[str, Any]]]:
        res = {}
        for source_file in files_data:
            try:
                if not source_file.filepath.strip():
                    continue
                res[source_file.fileid] = self.__get_source_lines(source_file.filepath)
            except FileNotFoundError:
                log.warning(f"Source file {source_file.filepath} was not found")
                continue
            except PermissionError:
                log.warning(
                    f"Permission denied for reading source file {source_file.filepath}"
                )
                continue
            except IsADirectoryError:
                log.warning(f"Source file {source_file.filepath} is a directory")
                continue
            except UnicodeDecodeError:
                continue

        for line in executed_source_lines:
            if line.fileid not in res:
                continue
            if line.linenum - 1 >= len(res[line.fileid]):
                log.warning(
                    f"Debug info line number {line.linenum} "
                    f"exceeds the number of lines "
                    f"in file with id {line.fileid}"
                )
                continue
            res[line.fileid][line.linenum - 1].update(
                {
                    "execount": line.execount,
                    "exetime": line.exetime,
                    "color_hotness": line.color_hotness,
                    "ubbankconflicts": line.ubbankconflicts,
                    "crossinstrbankconflicts": line.crossinstrbankconflicts,
                    "vecutil": line.vecutil,
                    "cache_line_count": line.cache_line_count,
                    "cache_line_utilization": line.cache_line_utilization,
                    "mem_access_stride": line.mem_access_stride,
                    "warp_occupancy": line.warp_occupancy,
                    "processed_bytes": line.processed_bytes,
                    "memory_traffic": line.memory_traffic,
                    "gp_registers": line.gp_registers,
                    "shared_registers": line.shared_registers,
                    "branch_efficiency": line.branch_efficiency,
                    "is_hint": line.is_hint,
                    "ipc": line.ipc,
                    "samples": line.samples,
                    "simd_samples": line.simd_samples,
                }
            )
        return res

    @staticmethod
    def __get_source_lines(file_path: str) -> List[Dict[str, Any]]:
        with open(file_path, "r") as file:
            lines = file.readlines()
        return [
            {
                "linenum": idx + 1,
                "name": line[:-1],
                "execount": "",
                "exetime": "",
                "ubbankconflicts": "",
                "crossinstrbankconflicts": "",
                "vecutil": "",
                "color_hotness": -1,
                "processed_bytes": "",
                "memory_traffic": "",
                "gp_registers": "",
                "shared_registers": "",
                "cache_line_count": "",
                "cache_line_utilization": "",
                "mem_access_stride": "",
                "branch_efficiency": "",
                "warp_occupancy": "",
                "is_hint": False,
                "ipc": "",
                "samples": "",
                "simd_samples": "",
            }
            for idx, line in enumerate(lines)
        ]

    @staticmethod
    def __create_call_graph_json(kernel_name):
        try:
            generator = CallGraphGenerator(kernel_name)
            return generator.create_call_graph_json()
        except ValueError:
            return {}

    @staticmethod
    def __get_active_metrics(
        assembly_json: List[Dict[str, Any]],
    ) -> List[Dict[str, Any]]:
        active_metrics = {}
        for instr in assembly_json:
            for id, val in instr.items():
                if id in MetricsDict and id not in active_metrics and val != "":
                    active_metrics[id] = {
                        **MetricsDict[id],  # type:ignore
                        "visible": True,
                        "id": id,
                    }
        active_metrics = {
            metric_name: active_metrics[metric_name]
            for metric_name in MetricsDict
            if metric_name in active_metrics
        }
        return list(active_metrics.values())
