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

import os
import json
import math
import sqlite3
import logging as log
import traceback
from typing import List, Any, Dict, Set, Tuple, Iterator, Optional

from npu_emulate.src.common.biprof_enum import MemoryType
from npu_emulate.src.common.biprof_constants import (
    MEM_UTIL_HEIGHT,
    MEM_UTIL_WIDTH,
    memory_size_map,
    MEMORY_ACCESS_READ,
)
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.common.utils import (
    is_in_pyinstaller_env,
    get_pyinstaller_path,
    decompose_instr_id,
)
from npu_emulate.src.common.stage_reporter import StageReporter


class DatabaseManager:
    @staticmethod
    def iter_mem_accesses(core_id: int) -> Iterator[sqlite3.Row]:
        return database_handler.iter_rows(
            """
        SELECT * FROM MemoryRWAccesses
        WHERE CoreId=:core_id
        AND ExecInstrId NOT NULL

        UNION ALL

        SELECT * FROM VFMemoryRWAccesses
        WHERE CoreId=:core_id
        ORDER BY ExecInstrId, AccessMode, MemoryType, AccessedAddress
        """,
            {"core_id": core_id},
        )

    @staticmethod
    def iter_instrs(core_id: int) -> Iterator[sqlite3.Row]:
        return database_handler.iter_rows(
            """
        SELECT EI.*,
        COALESCE(DmaMovProcessedBytes.ProcessedBytes,
                 FixpParams.ProcessedBytes) AS ProcessedBytes
        FROM ExecutedInstructions AS EI
        LEFT JOIN DmaMovProcessedBytes
        ON DmaMovProcessedBytes.ExecInstrId = EI.ExecInstrId
        LEFT JOIN FixpParams
        ON FixpParams.ExecInstrId = EI.ExecInstrId
        WHERE ExecInstrCoreId=:core_id
        """,
            {"core_id": core_id},
        )


class MatrixCopyingVizualizerHTMLGenerator:
    def __init__(self) -> None:
        self.instrs: Dict[int, Dict[str, Any]] = {}

    @staticmethod
    def addresses_to_ranges(addrs: List[int]):
        addrs = sorted(addrs)
        ranges: List[Dict[str, int]] = []
        if not addrs:
            return ranges
        start = addrs[0]
        prev = addrs[0]
        for addr in addrs[1:]:
            if addr == prev:
                continue
            if addr == prev + 1:
                prev = addr
            else:
                ranges.append({"start": start, "len": prev - start + 1})
                start = addr
                prev = addr
        ranges.append({"start": start, "len": prev - start + 1})
        return ranges

    @staticmethod
    def count_blank_lines(line_numbers: Set[int]) -> int:
        res = 0
        if len(line_numbers) < 2:
            return res
        lines_list = list(line_numbers)
        lines_list.sort()
        for i in range(1, len(lines_list)):
            diff = lines_list[i] - lines_list[i - 1]
            if diff > 1:
                res += 1
        return res

    def get_instrs_params(self, core: int) -> None:
        instrs = DatabaseManager.iter_instrs(core)
        for instr in instrs:
            instr_id = decompose_instr_id(instr["ExecInstrId"])[0]
            self.instrs[instr_id] = {
                "instr_id": instr_id,
                "instr_name": instr["ExecInstrName"],
                "instr_params": instr["ExecInstrParams"],
                "processed_bytes": instr["ProcessedBytes"],
                "core_id": core,
                "core_type_id": instr["CoreTypeId"],
                "read_cells": set(),
                "write_cells": set(),
            }

    def check_and_recalc_rat(self, rat: int, orig_cell_ids: Set[int]) -> bool:
        new_out_line_numbers = set()
        for cell_id in orig_cell_ids:
            new_out_line_numbers.add(cell_id // rat // MEM_UTIL_WIDTH)
        blank_lines_count = self.count_blank_lines(new_out_line_numbers)
        return len(new_out_line_numbers) + blank_lines_count > MEM_UTIL_HEIGHT

    def get_out_ratio(self, orig_cell_ids: Set[int]) -> int:
        rat = 1
        out_line_numbers = set()
        for cell_id in orig_cell_ids:
            out_line_numbers.add(cell_id // MEM_UTIL_WIDTH)
        blank_lines_count = self.count_blank_lines(out_line_numbers)
        if len(out_line_numbers) + blank_lines_count > MEM_UTIL_HEIGHT:
            rat = math.ceil(
                (len(out_line_numbers) + blank_lines_count) / MEM_UTIL_HEIGHT
            )
            rat = 1 if rat == 0 else 2 ** (rat - 1).bit_length()
        while self.check_and_recalc_rat(rat, orig_cell_ids):
            rat *= 2
        return rat

    @staticmethod
    def _scale_cells(cells: Set[int], rat: int) -> Set[int]:
        return {cell // rat for cell in cells}

    def update_out_cell_id(self, read_rat: int, write_rat: int) -> None:
        for key in self.instrs:
            if (
                "read_mem_type" in self.instrs[key]
                and self.instrs[key]["read_mem_type"] == "OUT"
            ):
                self.instrs[key]["read_cell_size"] *= read_rat
                self.instrs[key]["read_cells"] = self._scale_cells(
                    self.instrs[key]["read_cells"], read_rat
                )
            if (
                "write_mem_type" in self.instrs[key]
                and self.instrs[key]["write_mem_type"] == "OUT"
            ):
                self.instrs[key]["write_cell_size"] *= write_rat
                self.instrs[key]["write_cells"] = self._scale_cells(
                    self.instrs[key]["write_cells"], write_rat
                )

    @staticmethod
    def map_old_to_new_lines(
        orig_cells: Set[int], cell_size: int
    ) -> Tuple[Dict[int, int], Dict[int, int]]:
        cells = sorted(list(orig_cells))
        old_to_new_map: Dict[int, int] = {}
        line_to_addr: Dict[int, int] = {}

        prev_old_line = cells[0] // MEM_UTIL_WIDTH
        prev_new_line = 0

        for cell in cells:
            current_old_line = cell // MEM_UTIL_WIDTH
            gap_lines = current_old_line - prev_old_line
            gap_lines = gap_lines if gap_lines < 3 else 2
            current_new_line = prev_new_line + gap_lines
            old_to_new_map[current_old_line] = current_new_line
            prev_old_line = current_old_line
            prev_new_line = current_new_line
            line_to_addr[current_new_line] = (
                current_old_line * MEM_UTIL_WIDTH * cell_size
            )
        return old_to_new_map, line_to_addr

    @staticmethod
    def process_out_lines(addrs: Dict[int, int]) -> Dict[int, str]:
        addrs_to_lines: Dict[int, Any] = {}
        for i in range(0, MEM_UTIL_HEIGHT):
            if i in addrs:
                addrs_to_lines[i] = hex(addrs[i]).replace("0x", "")
            else:
                addrs_to_lines[i] = "         "
        return addrs_to_lines

    def update_out_lines(self) -> Tuple[Dict[int, str], Dict[int, str]]:
        orig_write_cells = set()
        orig_read_cells = set()
        out_read_cell_size = 1
        out_write_cell_size = 1
        for key in self.instrs:
            if (
                "read_mem_type" in self.instrs[key]
                and self.instrs[key]["read_mem_type"] == "OUT"
            ):
                orig_read_cells.update(self.instrs[key]["read_cells"])
                out_read_cell_size = self.instrs[key]["read_cell_size"]
            if (
                "write_mem_type" in self.instrs[key]
                and self.instrs[key]["write_mem_type"] == "OUT"
            ):
                orig_write_cells.update(self.instrs[key]["write_cells"])
                out_write_cell_size = self.instrs[key]["write_cell_size"]
        read_map, read_addrs = self.map_old_to_new_lines(
            orig_read_cells, out_read_cell_size
        )
        write_map, write_addrs = self.map_old_to_new_lines(
            orig_write_cells, out_write_cell_size
        )

        for key in self.instrs:
            if (
                "read_mem_type" in self.instrs[key]
                and self.instrs[key]["read_mem_type"] == "OUT"
            ):
                new_read_cells = set()
                for cell in self.instrs[key]["read_cells"]:
                    new_val = (
                        read_map[cell // MEM_UTIL_WIDTH] * MEM_UTIL_WIDTH
                        + cell % MEM_UTIL_WIDTH
                    )
                    new_read_cells.add(new_val)
                self.instrs[key]["read_cells"] = new_read_cells
            if (
                "write_mem_type" in self.instrs[key]
                and self.instrs[key]["write_mem_type"] == "OUT"
            ):
                new_write_cells = set()
                for cell in self.instrs[key]["write_cells"]:
                    new_val = (
                        write_map[cell // MEM_UTIL_WIDTH] * MEM_UTIL_WIDTH
                        + cell % MEM_UTIL_WIDTH
                    )
                    new_write_cells.add(new_val)
                self.instrs[key]["write_cells"] = new_write_cells
        read_lines = self.process_out_lines(read_addrs)
        write_lines = self.process_out_lines(write_addrs)
        return read_lines, write_lines

    def get_access_addrs_for_instrs(
        self, core: int
    ) -> Tuple[Dict[int, str], Dict[int, str]]:
        mem_accesses = DatabaseManager.iter_mem_accesses(core)
        out_read_cell_ids_size32 = set()
        out_write_cell_ids_size32 = set()
        OUT_CELL_SIZE = 32
        for item in mem_accesses:
            instr_id = decompose_instr_id(item["ExecInstrId"])[0]
            mem_type = MemoryType(item["MemoryType"])
            mem_type_name = mem_type.name.replace("_", " ")
            mem_size = memory_size_map[biprof_ctx.arch][mem_type]
            cell_size = (
                mem_size // MEM_UTIL_HEIGHT // MEM_UTIL_WIDTH
                if mem_type != MemoryType.OUT
                else OUT_CELL_SIZE
            )
            is_read = item["AccessMode"] == MEMORY_ACCESS_READ
            addr = item["AccessedAddress"]
            cell_id = addr // cell_size
            if mem_type == MemoryType.OUT and is_read:
                out_read_cell_ids_size32.add(cell_id)
            elif mem_type == MemoryType.OUT and not is_read:
                out_write_cell_ids_size32.add(cell_id)
            if instr_id not in self.instrs:
                continue
            if is_read:
                if "read_cell_size" not in self.instrs[instr_id]:
                    self.instrs[instr_id].update(
                        {
                            "read_cell_size": cell_size,
                            "read_mem_type": mem_type_name,
                            "read_mem_size": mem_size,
                        }
                    )
                self.instrs[instr_id]["read_cells"].add(cell_id)
            else:
                if "write_cell_size" not in self.instrs[instr_id]:
                    self.instrs[instr_id].update(
                        {
                            "write_cell_size": cell_size,
                            "write_mem_type": mem_type_name,
                            "write_mem_size": mem_size,
                        }
                    )
                # Sometimes FIX_L0C_TO_UB has 2 destinations (AIV0 and AIV1 UB),
                # so we need to divide such instructions in 2 frames:
                # 1 frame:
                # src (read_mem_type): L0C, dst (write_mem_type): AIV0_UB
                # 2 frame:
                # src (read_mem_type): L0C, dst (write_mem_type): AIV1_UB
                elif (
                    self.instrs[instr_id]["write_mem_type"] != mem_type_name
                    and "write_dict" not in self.instrs[instr_id]
                ):
                    write_params = self.instrs[instr_id]
                    self.instrs[instr_id]["write_dict"] = {
                        self.instrs[instr_id]["write_mem_type"]: {
                            "write_cell_size": write_params["write_cell_size"],
                            "write_mem_type": write_params["write_mem_type"],
                            "write_mem_size": write_params["write_mem_size"],
                            "write_cells": write_params["write_cells"],
                        },
                        mem_type_name: {
                            "write_cell_size": cell_size,
                            "write_mem_type": mem_type_name,
                            "write_mem_size": mem_size,
                            "write_cells": set(),
                        },
                    }
                if "write_dict" not in self.instrs[instr_id]:
                    self.instrs[instr_id]["write_cells"].add(cell_id)
                else:
                    self.instrs[instr_id]["write_dict"][mem_type_name][
                        "write_cells"
                    ].add(cell_id)
        read_rat = self.get_out_ratio(out_read_cell_ids_size32)
        write_rat = self.get_out_ratio(out_write_cell_ids_size32)
        self.update_out_cell_id(read_rat, write_rat)
        return self.update_out_lines()

    def process_cells(self) -> None:
        for key in list(self.instrs.keys()):
            if (
                "read_cell_size" not in self.instrs[key]
                or "write_cell_size" not in self.instrs[key]
            ):
                del self.instrs[key]
                continue
            self.instrs[key]["read_ranges"] = self.addresses_to_ranges(
                list(self.instrs[key]["read_cells"])
            )
            del self.instrs[key]["read_cells"]
            if "write_dict" not in self.instrs[key]:
                self.instrs[key]["write_ranges"] = self.addresses_to_ranges(
                    list(self.instrs[key]["write_cells"])
                )
                del self.instrs[key]["write_cells"]
            else:
                for mem_key in self.instrs[key]["write_dict"]:
                    self.instrs[key]["write_dict"][mem_key]["write_ranges"] = (
                        self.addresses_to_ranges(
                            list(self.instrs[key]["write_dict"][mem_key]["write_cells"])
                        )
                    )
                    del self.instrs[key]["write_dict"][mem_key]["write_cells"]

    def process_instrs_with_two_dests(self) -> List[Any]:
        instrs_json = []
        for instr in list(self.instrs.values()):
            if "write_dict" not in instr:
                instrs_json.append(instr)
            else:
                for item in list(instr["write_dict"].values()):
                    instrs_json.append(
                        {
                            "instr_id": instr["instr_id"],
                            "instr_name": instr["instr_name"],
                            "instr_params": instr["instr_params"],
                            "processed_bytes": instr["processed_bytes"],
                            "core_id": instr["core_id"],
                            "core_type_id": instr["core_type_id"],
                            "read_cell_size": instr["read_cell_size"],
                            "read_mem_type": instr["read_mem_type"],
                            "read_mem_size": instr["read_mem_size"],
                            "read_ranges": instr["read_ranges"],
                            "write_cell_size": item["write_cell_size"],
                            "write_mem_type": item["write_mem_type"],
                            "write_mem_size": item["write_mem_size"],
                            "write_ranges": item["write_ranges"],
                        }
                    )
        return instrs_json

    @staticmethod
    def __get_html_template() -> Optional[str]:
        if is_in_pyinstaller_env():
            base_path = get_pyinstaller_path()
        else:
            base_path = os.path.join(os.path.dirname(__file__), "..", "..")
        path_to_template = os.path.abspath(
            os.path.join(base_path, "static/matrixCopyTemplate.html")
        )
        if not os.path.isfile(path_to_template):
            return None
        with open(path_to_template, "r") as temp:
            text_temp = temp.read()
        return text_temp

    def __get_json_data(self, core_id: int) -> str:
        out_lines_json = []
        self.instrs.clear()
        self.get_instrs_params(core_id)
        read_lines, write_lines = self.get_access_addrs_for_instrs(core_id)
        out_lines_json.append(
            {
                "core_id": core_id,
                "out_read_lines": read_lines,
                "out_write_lines": write_lines,
            }
        )
        self.process_cells()
        instrs_json = self.process_instrs_with_two_dests()
        res = {
            "instrs": instrs_json,
            "out_lines": out_lines_json,
        }
        json_data = json.dumps(res)
        return json_data

    def prepare_report(self, output_dir: str) -> int:
        try:
            with StageReporter("Matrix copy visualizer: getting HTML template"):
                html_temp = self.__get_html_template()
            if not html_temp:
                log.warning("Matrix Copy HTML template not found")
                return 0
            for core in BackendDBManager.get_active_cores():
                with StageReporter(
                    f"Matrix copy visualizer (core {core}): "
                    "getting JSON data, injecting and "
                    "writing data to file"
                ):
                    json_data = self.__get_json_data(core)
                    html_text = html_temp.replace(
                        "window.MATRIX_COPY_DATA = null;",
                        f"window.MATRIX_COPY_DATA = {json_data};",
                    )
                    with open(
                        f"{output_dir}/core_{core}_matrix_copy_visualizer.html", "w+"
                    ) as output:
                        output.write(html_text)
            ret = 0
        except TypeError as error:
            log.error(str(error))
            log.debug(traceback.format_exc())
            ret = 1
        except ValueError as error:
            log.warning("Memory copy visualizer skipped")
            log.warning(str(error))
            ret = 0
        return ret
