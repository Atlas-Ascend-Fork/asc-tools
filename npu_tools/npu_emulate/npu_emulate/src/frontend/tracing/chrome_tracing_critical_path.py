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

from typing import Any, List
from npu_emulate.src.frontend.tracing.chrome_tracing_default import (
    CHROME_COLOR_GREY,
    ChromeTracingBuilder,
    InstrTraceBuilder,
    DatabaseManager,
)
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.common.biprof_context import biprof_ctx
from concurrent.futures import ProcessPoolExecutor, as_completed
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.logger import final_logger


class DatabaseManagerExtra:
    @staticmethod
    def get_cpath_instr_list() -> List[int]:
        if not database_handler.table_exists("DispatchTime"):
            return []
        cpath_instrs = database_handler.execute("""
        SELECT InstrId
        FROM CriticalPath
        """).fetchall()
        return [int(instr["InstrId"]) for instr in cpath_instrs]


class CriticalPathTraceBuilder(InstrTraceBuilder):
    def __init__(self) -> None:
        super().__init__()
        self.cpath_instr_list = DatabaseManagerExtra.get_cpath_instr_list()
        self.dispatch_events_map = {}
        event_map_copy = dict(self.event_details_map)
        for key in event_map_copy.keys():
            if key not in self.cpath_instr_list:
                self.event_details_map.pop(key)

    def get_dispatch_events(
        self, pid, pipe, core_id, core_type_id, instr_type_id
    ) -> List[Any]:
        return []

    def get_color(self, instr: Any) -> str:
        if instr["Id"] in self.cpath_instr_list:
            return ""
        return CHROME_COLOR_GREY


class CriticalPathBuilder(ChromeTracingBuilder):
    @staticmethod
    def get_json_obj(core_id: int) -> List[Any]:
        res = CriticalPathTraceBuilder().get_json_obj(core_id)
        return res

    @classmethod
    def generate(cls, output_dir) -> int:
        core_list = DatabaseManager.get_active_core_list()
        with StageReporter("Critical path generation"):
            with ProcessPoolExecutor(biprof_ctx.jobs) as executor:
                futures = [
                    executor.submit(
                        cls.generate_core_tracing, core, output_dir, "critical_path"
                    )
                    for core in core_list
                ]
                for future in as_completed(futures):
                    result = future.result()
                    if result != 0:
                        return 1
        final_logger.add_general_info(f"Critical path reports: {output_dir}")
        return 0
