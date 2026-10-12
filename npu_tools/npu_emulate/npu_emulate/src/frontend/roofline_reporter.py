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

import dataclasses
import json
import logging as log
import os
import re
import traceback
from collections import defaultdict
from dataclasses import dataclass
from typing import Any, Dict, Tuple, Optional

from npu_emulate.src.arch_diagram.constants import MEMORY_BANDWIDTH_V100
from npu_emulate.src.backend.analyzers.david.fixp_analyzer import (
    AIV0_UB_MASK,
    AIV1_UB_MASK,
)
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.common.biprof_constants import (
    BYTES_IN_GB,
    core_num_map,
    frequency_ghz_map,
    peak_tflops_map,
)
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.common.biprof_enum import (
    ArchName,
    CoreType,
    InstrType,
    UbAccessType,
)
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.common.utils import (
    get_param_value,
    get_pyinstaller_path,
    is_in_pyinstaller_env,
)


@dataclass
class FuncStats:
    pc: int
    func_size: int  # bytes
    unit: str
    exec_count: int
    duration_sum: int
    proc_elem_sum: int
    int_elem_sum: int


@dataclass
class FuncStatsByLabel:
    proc_bytes_sum: int
    arithmetic_intensity: float
    throughput: float
    performance: float


class RooflineReporter:
    vf_name_match = re.compile(r"VF(_SIMT)? .* <(?P<name>.*)>")

    def __init__(self) -> None:
        self.funcs_stats: Dict[str, FuncStats] = {}
        self.stats_by_label: Dict[Tuple[str, str], FuncStatsByLabel] = defaultdict(
            lambda: FuncStatsByLabel(0, 0.0, 0.0, 0.0)
        )

    def get_functions(self) -> Dict[str, FuncStats]:
        funcs = database_handler.execute("""
        SELECT * FROM Functions
        """).fetchall()
        return {
            func["FunctionName"]: FuncStats(
                func["SourceInstrAddr"], func["FunctionSize"], "AIC", 0, 0, 0, 0
            )
            for func in funcs
        }

    def find_func_for_instr(self, pc: int) -> str:
        for func, stat in self.funcs_stats.items():
            if stat.pc <= pc < stat.pc + stat.func_size:
                return func
        return ""

    def get_processed_elems_simd(self) -> None:
        simd_exec_instrs = database_handler.execute(
            """
        SELECT ExecInstrName, ExecInstrParams,
               SourceInstrAddr, ProcessedElements FROM ExecutedInstructions
        LEFT JOIN VectorUtilizations
        ON VectorUtilizations.ExecInstrId = ExecutedInstructions.ExecInstrId
        WHERE InstrTypeId=:instr_type_id
        AND ProcessedElements IS NOT NULL
        """,
            {"instr_type_id": InstrType.RVECEX.value},
        ).fetchall()
        for instr in simd_exec_instrs:
            func_name = self.find_func_for_instr(instr["SourceInstrAddr"])
            if not func_name:
                log.error(f"Cannot find VF function for instruction {dict(instr)}")
                continue
            params = instr["ExecInstrParams"]
            dtype = get_param_value("Dtype", params) if params else None
            is_int = dtype is not None and (
                dtype.startswith("u") or dtype.startswith("s")
            )
            if instr["ExecInstrName"] in ["RV_VMULA", "RV_VMADD"]:
                self.funcs_stats[func_name].proc_elem_sum += (
                    instr["ProcessedElements"] * 2
                )
                if is_int:
                    self.funcs_stats[func_name].int_elem_sum += (
                        instr["ProcessedElements"] * 2
                    )
            else:
                self.funcs_stats[func_name].proc_elem_sum += instr["ProcessedElements"]
                if is_int:
                    self.funcs_stats[func_name].int_elem_sum += instr[
                        "ProcessedElements"
                    ]

    def get_processed_elems_simt(self) -> None:
        simt_exec_instrs = database_handler.execute(
            """
        SELECT ExecInstrName, FullMask,
               SourceInstrAddr FROM SIMTExecutionMasks
        LEFT JOIN ExecutedInstructions
        ON SIMTExecutionMasks.ExecInstrId = ExecutedInstructions.ExecInstrId
        WHERE InstrTypeId=:instr_type_id
        """,
            {"instr_type_id": InstrType.RVECEX.value},
        ).fetchall()
        for instr in simt_exec_instrs:
            func_name = self.find_func_for_instr(instr["SourceInstrAddr"])
            if not func_name:
                log.error(f"Cannot find VF function for instruction {dict(instr)}")
                continue
            instr_name = str(instr["ExecInstrName"])
            elem_count = bin(instr["FullMask"]).count("1")
            f_instrs = ["SIMT_MUFU"]
            i_instrs = [
                "SIMT_LEA",
                "SIMT_LEA_HI_X",
                "SIMT_SHF",
                "SIMT_SHFI",
                "SIMT_PLOP3",
                "SIMT_LOP3",
                "SIMT_LOP3_I",
            ]
            if instr_name.startswith("SIMT_F") or instr_name in f_instrs:
                self.funcs_stats[func_name].proc_elem_sum += elem_count
                if instr_name.startswith("SIMT_FFMA"):
                    self.funcs_stats[func_name].proc_elem_sum += elem_count
            if instr_name.startswith("SIMT_I") or instr_name in i_instrs:
                self.funcs_stats[func_name].proc_elem_sum += elem_count
                self.funcs_stats[func_name].int_elem_sum += elem_count
                if instr_name.startswith("SIMT_IMAD"):
                    self.funcs_stats[func_name].proc_elem_sum += elem_count
                    self.funcs_stats[func_name].int_elem_sum += elem_count

    def get_processed_elems_cube(self) -> None:
        cube_exec_instrs = database_handler.execute("""
        SELECT SourceInstrAddr, M, K, N, MatrixCDtype FROM MmadParams
        LEFT JOIN ExecutedInstructions
        ON ExecutedInstructions.ExecInstrId = MmadParams.ExecInstrId
        """).fetchall()
        for instr in cube_exec_instrs:
            func_name = self.find_func_for_instr(instr["SourceInstrAddr"])
            if not func_name:
                log.error(f"Cannot find VF function for instruction {dict(instr)}")
                continue
            elem_count = instr["M"] * instr["N"] * instr["K"] * 2
            dtype = instr["MatrixCDtype"]
            self.funcs_stats[func_name].proc_elem_sum += elem_count
            if dtype is not None and dtype.startswith(("u", "s")):
                self.funcs_stats[func_name].int_elem_sum += elem_count

    def get_processed_bytes_simd(self) -> None:
        simd_ld_st_instrs = database_handler.execute(
            """
        SELECT SourceInstrAddr, ProcessedBytes FROM ExecutedInstructions
        LEFT JOIN VectorUtilizations
        ON VectorUtilizations.ExecInstrId = ExecutedInstructions.ExecInstrId
        WHERE (InstrTypeId=:rvecld_type_id OR InstrTypeId=:rvecst_type_id)
        AND ProcessedBytes IS NOT NULL
        """,
            {
                "rvecld_type_id": InstrType.RVECLD.value,
                "rvecst_type_id": InstrType.RVECST.value,
            },
        ).fetchall()
        for instr in simd_ld_st_instrs:
            func_name = self.find_func_for_instr(instr["SourceInstrAddr"])
            if not func_name:
                log.error(f"Cannot find VF function for instruction {dict(instr)}")
                continue
            self.stats_by_label[(func_name, "SIMD")].proc_bytes_sum += instr[
                "ProcessedBytes"
            ]

    def get_processed_bytes_simt(self) -> None:
        simt_ub_instrs = database_handler.execute(
            """
        SELECT SourceInstrAddr, ProcessedBytes
        FROM UbRwStatistics AS UBS
        LEFT JOIN ExecutedInstructions AS EI
        ON UBS.ExecInstrId = EI.ExecInstrId
        WHERE (UBS.UbAccessType=:simt_rd OR UBS.UbAccessType=:simt_wr)
        """,
            {
                "simt_rd": UbAccessType.SIMT_RD.value,
                "simt_wr": UbAccessType.SIMT_WR.value,
            },
        ).fetchall()
        for instr in simt_ub_instrs:
            func_name = self.find_func_for_instr(instr["SourceInstrAddr"])
            if not func_name:
                log.error(f"Cannot find VF function for instruction {dict(instr)}")
                continue
            self.stats_by_label[(func_name, "SIMT")].proc_bytes_sum += instr[
                "ProcessedBytes"
            ]

    def get_processed_bytes_simt_out(self) -> None:
        simt_out_ld_st_instrs = database_handler.execute("""
        SELECT SourceInstrAddr, ProcessedBytes FROM SIMTLdStProcessedBytes
        LEFT JOIN ExecutedInstructions AS EI
        ON EI.ExecInstrId = SIMTLdStProcessedBytes.ExecInstrId
        WHERE EI.ExecInstrName = 'SIMT_STG' OR EI.ExecInstrName = 'SIMT_LDG'
        """).fetchall()
        for instr in simt_out_ld_st_instrs:
            func_name = self.find_func_for_instr(instr["SourceInstrAddr"])
            if not func_name:
                log.error(f"Cannot find VF function for instruction {dict(instr)}")
                continue
            self.stats_by_label[(func_name, "SIMT-OUT")].proc_bytes_sum += instr[
                "ProcessedBytes"
            ]

    def get_processed_bytes_cube_l0(self) -> None:
        cube_ld_st_instrs = database_handler.execute(
            """
        SELECT SourceInstrAddr,
        COALESCE(DMPB.ProcessedBytes, 0) + COALESCE(FP.ProcessedBytes, 0)
        AS ProcessedBytes FROM ExecutedInstructions AS EI
        LEFT JOIN DmaMovProcessedBytes AS DMPB
        ON DMPB.ExecInstrId = EI.ExecInstrId
        LEFT JOIN FixpParams AS FP
        ON FP.ExecInstrId = EI.ExecInstrId
        WHERE (DMPB.ProcessedBytes IS NOT NULL
        OR FP.ProcessedBytes IS NOT NULL)
        AND (InstrTypeId=:mte2_id OR InstrTypeId=:fixp_id)
        """,
            {
                "mte2_id": InstrType.MTE2.value,
                "fixp_id": InstrType.FIXP.value,
            },
        ).fetchall()
        for instr in cube_ld_st_instrs:
            func_name = self.find_func_for_instr(instr["SourceInstrAddr"])
            if not func_name:
                log.error(f"Cannot find function for instruction {dict(instr)}")
                continue
            self.stats_by_label[(func_name, "L0")].proc_bytes_sum += instr[
                "ProcessedBytes"
            ]

    def get_processed_bytes_cube_out(self) -> None:
        cube_out_l1_instrs = database_handler.execute(
            """
        SELECT SourceInstrAddr, ProcessedBytes
        FROM DmaMovProcessedBytes AS DMPB
        LEFT JOIN ExecutedInstructions AS EI
        ON DMPB.ExecInstrId = EI.ExecInstrId
        WHERE InstrTypeId = :mte2
        AND ExecInstrParams LIKE 'Src:OUT, Dst:L1%'
        """,
            {"mte2": InstrType.MTE2.value},
        ).fetchall()
        for instr in cube_out_l1_instrs:
            func_name = self.find_func_for_instr(instr["SourceInstrAddr"])
            if not func_name:
                log.error(f"Cannot find function for instruction {dict(instr)}")
                continue
            self.stats_by_label[(func_name, "CUBE-OUT")].proc_bytes_sum += instr[
                "ProcessedBytes"
            ]

        cube_out_fixp_instrs = database_handler.execute("""
        SELECT SourceInstrAddr, ProcessedBytes, DstUBMask
        FROM ExecutedInstructions AS EI
        LEFT JOIN FixpParams AS FP
        ON FP.ExecInstrId = EI.ExecInstrId
        WHERE ExecInstrParams LIKE 'Src:L0C32, Dst:OUT%'
        """).fetchall()
        for instr in cube_out_fixp_instrs:
            func_name = self.find_func_for_instr(instr["SourceInstrAddr"])
            if not func_name:
                log.error(f"Cannot find function for instruction {dict(instr)}")
                continue
            pb = instr["ProcessedBytes"]
            if instr["DstUBMask"] == AIV0_UB_MASK | AIV1_UB_MASK:
                pb /= 2
            self.stats_by_label[(func_name, "CUBE-OUT")].proc_bytes_sum += pb

    def get_durations_vector(self) -> None:
        vf_instrs = database_handler.execute("""
        SELECT * FROM ExecutedInstructions
        LEFT JOIN SourceInstructions
        ON SourceInstructions.SourceInstrAddr =
           ExecutedInstructions.SourceInstrAddr
        WHERE ExecInstrName LIKE '%VF%'
        """).fetchall()

        for vf in vf_instrs:
            if vf["SourceInstrText"] is None:
                continue
            vf_match = self.vf_name_match.search(vf["SourceInstrText"])
            if vf_match:
                name = vf_match.group("name")
                if name not in self.funcs_stats:
                    continue
                self.funcs_stats[name].duration_sum += (
                    vf["ExecInstrTickEnd"] - vf["ExecInstrTickStart"]
                )
                self.funcs_stats[name].exec_count += 1
                self.funcs_stats[name].unit = "AIV"

    def get_durations_cube(self) -> None:
        for func in self.funcs_stats:
            durations = database_handler.execute(
                """
            SELECT COALESCE(MIN(ExecInstrTickStart), 0) as MinTick,
                   COALESCE(MAX(ExecInstrTickEnd), 0) as MaxTick
            FROM ExecutedInstructions
            WHERE SourceInstrAddr >= :addr_lb
              AND SourceInstrAddr < :addr_ub
              AND CoreTypeId = :core_type_id
            """,
                {
                    "addr_lb": self.funcs_stats[func].pc,
                    "addr_ub": self.funcs_stats[func].pc
                    + self.funcs_stats[func].func_size,
                    "core_type_id": CoreType.AIC.value,
                },
            ).fetchone()
            if durations["MaxTick"] - durations["MinTick"] > 0:
                self.funcs_stats[func].duration_sum = (
                    durations["MaxTick"] - durations["MinTick"]
                )
                self.funcs_stats[func].exec_count += 1

    def get_data(self) -> None:
        self.get_processed_elems_simd()
        self.get_processed_elems_simt()
        self.get_processed_elems_cube()
        self.get_processed_bytes_simd()
        self.get_processed_bytes_simt()
        self.get_processed_bytes_simt_out()
        self.get_processed_bytes_cube_l0()
        self.get_processed_bytes_cube_out()
        self.get_durations_vector()
        self.get_durations_cube()

    def calculate_performance(self) -> None:
        self.stats_by_label = {
            (func, label): value
            for (func, label), value in self.stats_by_label.items()
            if self.funcs_stats[func].exec_count > 0
            and self.funcs_stats[func].duration_sum > 0
            and self.funcs_stats[func].proc_elem_sum > 0
            and value.proc_bytes_sum
        }
        for func, label in self.stats_by_label:
            self.stats_by_label[(func, label)].arithmetic_intensity = (
                self.funcs_stats[func].proc_elem_sum
                / self.stats_by_label[(func, label)].proc_bytes_sum
            )
            self.stats_by_label[(func, label)].throughput = (
                self.stats_by_label[(func, label)].proc_bytes_sum
                / BYTES_IN_GB
                / (
                    self.funcs_stats[func].duration_sum
                    / (frequency_ghz_map[ArchName[biprof_ctx.arch.upper()]] * 10**9)
                )
            )
            self.stats_by_label[(func, label)].performance = (
                self.stats_by_label[(func, label)].arithmetic_intensity
                * self.stats_by_label[(func, label)].throughput
            )

    def prepare_report(self, output_dir: str) -> int:
        try:
            with StageReporter("Roofline reporter: getting HTML template"):
                html_temp = self.__get_html_template()
                if not html_temp:
                    log.warning("Roofline HTML template not found")
                    return 0
            active_core_num = len(BackendDBManager.get_active_cores())
            with StageReporter(
                "Roofline reporter: "
                "getting JSON data, injecting and "
                "writing data to file"
            ):
                kernel_data = self.__get_json_kernel_data()
                arch_data = self.__get_json_arch_data(active_core_num)

                if not kernel_data or not arch_data:
                    log.warning("Roofline reporter skipped")
                    return 0

                html_text = html_temp.replace("%%ARCHITECTURES_JSON%%", f"{arch_data};")
                html_text = html_text.replace("%%KERNELS_JSON%%", f"{kernel_data}")
                with open(f"{output_dir}/roofline_perf_model.html", "w+") as output:
                    output.write(html_text)
            ret = 0
        except TypeError as error:
            log.error(str(error))
            log.debug(traceback.format_exc())
            ret = 1
        except ValueError as error:
            log.warning("Roofline reporter skipped")
            log.warning(str(error))
            ret = 0
        return ret

    def __get_html_template(self) -> Optional[str]:
        if is_in_pyinstaller_env():
            base_path = get_pyinstaller_path()
        else:
            base_path = os.path.join(os.path.dirname(__file__), "..", "..")
        path_to_template = os.path.abspath(
            os.path.join(base_path, "static/rooflineTemplate.html")
        )

        if not os.path.isfile(path_to_template):
            return None

        with open(path_to_template) as temp:
            return temp.read()

    def __get_json_arch_data(self, active_core_num: int) -> str:
        arch: ArchName = ArchName[biprof_ctx.arch.upper()]
        res_current_arch: Dict[str, Any] = {
            "id": "arch_1",
            "name": "David " + arch.name,
            "active_cores_count": active_core_num,
            "max_cores_num": core_num_map[arch],
        }
        simd_bandwidth = 0.0
        simt_ub_bandwidth = 0.0
        simt_out_bandwidth = 0.0
        cube_l0_bandwidth = 0.0
        cube_out_bandwidth = 0.0
        if arch == ArchName.V100:
            # Do NOT sum Reads + Writes in v100, use max Reads (512B) only
            simd_bandwidth = (
                MEMORY_BANDWIDTH_V100["aiv0_ub_to_simd"]
                + MEMORY_BANDWIDTH_V100["aiv1_ub_to_simd"]
            )
            # Do NOT sum Reads + Writes in v100, use max Reads (128B) only
            simt_ub_bandwidth = (
                MEMORY_BANDWIDTH_V100["aiv0_ub_to_simt"]
                + MEMORY_BANDWIDTH_V100["aiv1_ub_to_simt"]
            )
            # Do NOT use per link bandwidth, use per chip bandwidth instead
            simt_out_bandwidth = MEMORY_BANDWIDTH_V100["chip_to_out"]
            cube_l0_bandwidth = (
                MEMORY_BANDWIDTH_V100["aic_l1_to_l0a"]
                + MEMORY_BANDWIDTH_V100["aic_l1_to_l0b"]
                + MEMORY_BANDWIDTH_V100["aic_l0c_to_all_syn"]
            )
            # Do NOT use per link bandwidth, use per chip bandwidth instead
            cube_out_bandwidth = MEMORY_BANDWIDTH_V100["chip_to_out"]
        simd_bandwidth *= core_num_map[arch]
        simt_ub_bandwidth *= core_num_map[arch]
        cube_l0_bandwidth *= core_num_map[arch]

        res_current_arch["AIV"] = {
            "vector_fp16": {
                "peakFlops": peak_tflops_map[arch]["vector_fp16_FMA"] * 1000,
                "peakBandwidthSIMD": simd_bandwidth,
                "peakBandwidthSIMT": simt_ub_bandwidth,
                "peakBandwidthSIMT_OUT": simt_out_bandwidth,
            },
            "vector_bf16": {
                "peakFlops": peak_tflops_map[arch]["vector_fp16_bf16"] * 1000,
                "peakBandwidthSIMD": simd_bandwidth,
                "peakBandwidthSIMT": simt_ub_bandwidth,
                "peakBandwidthSIMT_OUT": simt_out_bandwidth,
            },
            "vector_fp32": {
                "peakFlops": peak_tflops_map[arch]["vector_fp32"] * 1000,
                "peakBandwidthSIMD": simd_bandwidth,
                "peakBandwidthSIMT": simt_ub_bandwidth,
                "peakBandwidthSIMT_OUT": simt_out_bandwidth,
            },
        }
        res_current_arch["AIC"] = {
            "cube_fp16_bf16": {
                "peakFlops": peak_tflops_map[arch]["cube_fp16_bf16"] * 1000,
                "peakBandwidthL0": cube_l0_bandwidth,
                "peakBandwidthCUBE_OUT": cube_out_bandwidth,
            },
            "cube_tf32_hf32": {
                "peakFlops": peak_tflops_map[arch]["cube_tf32_hf32"] * 1000,
                "peakBandwidthL0": cube_l0_bandwidth,
                "peakBandwidthCUBE_OUT": cube_out_bandwidth,
            },
            "cube_fp32": {
                "peakFlops": peak_tflops_map[arch]["cube_fp32"] * 1000,
                "peakBandwidthL0": cube_l0_bandwidth,
                "peakBandwidthCUBE_OUT": cube_out_bandwidth,
            },
            "cube_int8_fp8": {
                "peakFlops": peak_tflops_map[arch]["cube_int8_fp8"] * 1000,
                "peakBandwidthL0": cube_l0_bandwidth,
                "peakBandwidthCUBE_OUT": cube_out_bandwidth,
            },
        }
        log.debug(f"Current arch: {res_current_arch}")
        return json.dumps(res_current_arch)

    def __get_json_kernel_data(self) -> str:
        res = {}

        self.funcs_stats = self.get_functions()
        self.get_data()
        self.calculate_performance()
        if not self.funcs_stats:
            log.debug("No roofline points")
            return ""
        for (func, label), stat in self.stats_by_label.items():
            res[f"{func} - {label}"] = {
                **dataclasses.asdict(self.funcs_stats[func]),
                **dataclasses.asdict(stat),
                "label": label,
            }
        log.debug(f"Roofline points: {res}")
        if not res:
            return ""
        return json.dumps(res)
