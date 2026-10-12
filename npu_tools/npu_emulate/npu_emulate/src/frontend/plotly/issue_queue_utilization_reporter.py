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
import math
import plotly.graph_objects as go  # type: ignore
from typing import List, Optional, Tuple
from npu_emulate.src.common.biprof_enum import CoreType
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.common.biprof_constants import SAMPLING_PIPES_TO_SUBCORES
from npu_emulate.src.frontend.base.plotly_reporter_base import PlotlyReporterBase


class IssueQueueUtilizationReporter(PlotlyReporterBase):
    @staticmethod
    def get_issue_queue_utilization_points(
        core_id: int,
        core_type_id: int,
        instr_type_id: int,
    ) -> Tuple[List[int], List[float], Optional[float], Optional[float]]:
        cur = database_handler.execute(
            """
            SELECT Tick, Util AS Value
            FROM IssueQueueUtilization
            WHERE CoreId = :core_id
              AND CoreTypeId = :core_type_id
              AND InstrTypeId = :instr_type_id
            ORDER BY Tick
        """,
            {
                "core_id": core_id,
                "core_type_id": core_type_id,
                "instr_type_id": instr_type_id,
            },
        )

        ticks: List[int] = []
        values: List[float] = []
        min_val = math.inf
        max_val = -math.inf

        for row in cur:
            t = row["Tick"]
            v = row["Value"]
            # Insert a leading zero anchor before the very first real point
            if not ticks and v != 0:
                ticks.append(t)
                values.append(0)
            ticks.append(t)
            values.append(v)
            if v < min_val:
                min_val = v
            if v > max_val:
                max_val = v

        if not ticks:
            return ticks, values, None, None

        # Trailing zero anchor
        if values[-1] != 0:
            ticks.append(ticks[-1])
            values.append(0)

        # The leading/trailing zeros affect the Y range too
        if min_val > 0:
            min_val = 0
        if max_val < 0:
            max_val = 0

        return ticks, values, min_val, max_val

    def _process(self) -> None:
        active_cores = BackendDBManager.get_active_cores()
        active_core_types = BackendDBManager.get_active_core_types_ordered()

        for core_id in active_cores:
            figures: List[go.Figure] = []

            for core_type_id in active_core_types:
                core_type = CoreType(core_type_id)
                core_type_name = core_type.name
                subcores = SAMPLING_PIPES_TO_SUBCORES[core_type]

                fig = go.Figure()
                min_tick = math.inf
                max_tick = -math.inf
                min_val = math.inf
                max_val = -math.inf

                for instr_type in subcores:
                    ticks, values, lo, hi = self.get_issue_queue_utilization_points(
                        core_id, core_type_id, instr_type.value
                    )
                    if not ticks:
                        continue

                    lo_tick = min(ticks)
                    hi_tick = max(ticks)
                    if lo_tick < min_tick:
                        min_tick = lo_tick
                    if hi_tick > max_tick:
                        max_tick = hi_tick

                    if lo is not None and lo < min_val:
                        min_val = lo
                    if hi is not None and hi > max_val:
                        max_val = hi

                    fig.add_trace(
                        go.Scatter(
                            x=ticks,
                            y=values,
                            mode="lines",
                            line_shape="hv",
                            name=instr_type.name,
                        )
                    )
                    # Release the local lists
                    # before the next subcore allocates new ones
                    del ticks, values

                if not fig.data:
                    continue

                fig.update_layout(
                    title=f"Issue Queue Utilization ({core_type_name})",
                    xaxis_title="Tick",
                    yaxis_title="Value",
                    xaxis=dict(range=[min_tick, max_tick]),
                    yaxis=dict(range=[min_val, max_val]),
                    width=None,
                    uirevision="fixed",
                )
                figures.append(fig)

            if not figures:
                continue

            report_file_name = f"qi_core_{core_id}_issue_queue_utilization_report.html"
            report_path = os.path.join(self.output_dir, report_file_name)
            self.save_multifigures_report(figures, report_path)

            figures.clear()

    def process(self) -> None:
        with StageReporter("[QI] Issue queue utilization report generation"):
            self._process()
        final_logger.add_general_info(f"Issue Queue reports: {self.output_dir}")
