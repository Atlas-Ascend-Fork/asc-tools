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

import logging as log
import os
import re
from typing import List, Optional

import npu_emulate.src.common.utils as utils
from npu_emulate.src.common.comm import print_error, print_info
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.backend.base.data_processors_base import IBaseReporter
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.analyzer_manager import AnalyzerManager
from npu_emulate.src.backend.hint_manager import HintManager
from npu_emulate.src.backend.hotspots_processor import HotspotsProcessor
from npu_emulate.src.backend.object_processor import ObjectProcessor
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.backend.vf_export import export_vf_report_json


class HotspotsReporterBase(IBaseReporter):
    def clear_tmp_files(self) -> None:
        for file in utils.get_files_by_suffix(self._output_dir, ".db"):
            os.remove(os.path.join(self._output_dir, file))

    @staticmethod
    def get_current_db_stem() -> str:
        db_path = database_handler.get_db_path()
        if db_path is None:
            return "vf_report"
        return os.path.splitext(os.path.basename(db_path))[0]


class HotspotsDbOnlyReporter(HotspotsReporterBase):
    def __init__(self) -> None:
        super().__init__()
        self._input_db: str = ""
        self._input_dbs: List[str] = []

    def init(self) -> int:
        ret = super().init()
        self._input_db = os.path.abspath(biprof_ctx.input_db)
        if os.path.isdir(self._input_db):
            self._input_dbs = sorted(
                os.path.join(self._input_db, name)
                for name in os.listdir(self._input_db)
                if name.endswith(".db")
            )
        elif os.path.isfile(self._input_db):
            self._input_dbs = [self._input_db]

        if not self._input_dbs:
            print_error(f"No contract DB files found in input-db: {self._input_db}")
            return 1
        return ret

    @staticmethod
    def _safe_report_component(value: str) -> str:
        value = value.strip() or "kernel"
        value = re.sub(r"[^\w.\-+=:@]+", "_", value)
        return value[:240] or "kernel"

    @staticmethod
    def _get_kernel_info_attr(attr: str) -> Optional[str]:
        if not database_handler.table_exists("KernelInfo"):
            return None
        row = database_handler.execute(
            """
            SELECT KernelInfoVal
            FROM KernelInfo
            WHERE KernelInfoAttr=:attr
            """,
            {"attr": attr},
        ).fetchone()
        if row is None:
            return None
        return str(row["KernelInfoVal"])

    @classmethod
    def _get_kernel_name(cls, input_db: str) -> str:
        name = cls._get_kernel_info_attr("name")
        if name:
            return name
        return os.path.splitext(os.path.basename(input_db))[0]

    @classmethod
    def _get_launch_id(cls, index: int) -> int:
        launch_id = cls._get_kernel_info_attr("launch_id")
        if not launch_id:
            return index
        try:
            return int(launch_id)
        except ValueError:
            return index

    def _process_one_db(self, input_db: str, index: int) -> int:
        ret = 0
        database_handler.connect(input_db)
        try:
            database_handler.create_tables(self._analysis_name)
            self._arch = database_handler.get_arch() or self._arch
            if biprof_ctx.arch is None and self._arch is not None:
                biprof_ctx.arch = self._arch
            BackendDBManager.fill_static_tables()

            kernel_name = self._get_kernel_name(input_db)
            launch_id = self._get_launch_id(index)
            report_kernel_name = self._safe_report_component(kernel_name)
            report_dir_name = f"{launch_id:04d}_{report_kernel_name}_reports"

            object_file = biprof_ctx.object_file
            if object_file:
                obj_proc = ObjectProcessor(self._arch, self._output_dir, object_file)
                functions, _ = obj_proc.read_object_functions()
                obj_data = obj_proc.read(kernel_name)
                with StageReporter("Save processed object info"):
                    BackendDBManager.save_processed_data(obj_data, functions)

            processor = HotspotsProcessor(
                arch=self._arch,
                workspace=self._output_dir,
                log_folder=os.path.join(
                    self._output_dir,
                    f"kernel_launch_{launch_id}",
                ),
                log_files=[],
                object_path=object_file,
                kernel_name=kernel_name,
                kernel_name_short=report_kernel_name,
                report_dir_name=report_dir_name,
            )

            analyzer = AnalyzerManager()
            analyzer.register_analyzers()
            analyzer.run_analyzers()

            hinter = HintManager()
            hinter.register_hints()
            hinter.run_hints()

            ret += processor.generate_chrome_tracing()
            ret += processor.generate_issue_queue_utilization_report()
            if biprof_ctx.matrix_copy:
                ret += processor.generate_mcv_html()
            if biprof_ctx.scalar_ipc:
                ret += processor.generate_scalar_ipc_dynamic_report()
            if biprof_ctx.vec_ipc or biprof_ctx.vf_report_json:
                ret += processor.generate_vf_ipc_reports()
            if biprof_ctx.vf_report_json:
                export_vf_report_json(
                    database_handler,
                    processor.report_dir_path,
                    self.get_current_db_stem(),
                )
            ret += processor.generate_instr_duration_distribution_report()
            ret += processor.generate_icache_report()
            if biprof_ctx.quantitative_indices:
                ret += processor.generate_memory_latencies_reports()
                ret += processor.generate_data_cache_report()
            if biprof_ctx.roofline:
                ret += processor.generate_roofline_report()
            if biprof_ctx.pmu:
                ret += processor.generate_scalar_pmu_performance_report()
            if biprof_ctx.simd_perf:
                ret += processor.generate_simd_performance_reports()
            if biprof_ctx.bubble:
                ret += processor.generate_bubble_report()
            ret += processor.generate_arch_diagram()

            ret += processor.generate_summary_report()
        finally:
            database_handler.disconnect()
        return ret

    def process(self) -> int:
        ret = 0
        print_info(f"Processing {len(self._input_dbs)} contract DB file(s).")
        for index, input_db in enumerate(self._input_dbs):
            if index > 0:
                StageReporter.separate()
            ret += self._process_one_db(input_db, index)

        if not biprof_ctx.save_tmp_files:
            self.clear_tmp_files()
        if ret != 0:
            print_error("Report generation failed!")
            log.error("Report generation failed!")
        return ret
