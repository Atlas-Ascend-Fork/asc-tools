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
import plotly.graph_objects as go  # type: ignore
from typing import List, Tuple
from npu_emulate.src.common.biprof_enum import CoreType
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.frontend.base.plotly_reporter_base import PlotlyReporterBase


class ScalarIPCDynamicReporter(PlotlyReporterBase):
    @staticmethod
    def get_scalar_ipc_dynamic_points(
        core_id: int, core_type_id: int
    ) -> Tuple[List[int], List[float]]:
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
        return list(range(1, len(rows) + 1)), [row["Value"] for row in rows]

    def _process(self) -> None:
        for core_id in BackendDBManager.get_active_cores():
            figures: List[go.Figure] = []

            core_type_ids = BackendDBManager.get_active_core_types_ordered()
            for core_type_id in core_type_ids:
                window_numbers, ipc_values = self.get_scalar_ipc_dynamic_points(
                    core_id, core_type_id
                )

                if not window_numbers or not ipc_values:
                    continue

                fig = go.Figure()
                fig.add_trace(
                    go.Scatter(
                        x=window_numbers,
                        y=ipc_values,
                        mode="lines",
                        name=f"{CoreType(core_type_id).name} - Core {core_id}",
                    )
                )
                fig.update_layout(
                    title=(
                        f"Scalar IPC Dynamic ({CoreType(core_type_id).name}), "
                        f"window size = {int(biprof_ctx.sliding_window)}"
                    ),
                    xaxis_title="Window Number",
                    yaxis_title="Value",
                    width=None,
                )

                figures.append(fig)

            if not figures:
                continue

            report_file_name = f"core_{core_id}_scalar_ipc_dynamic_report.html"
            report_path = os.path.join(self.output_dir, report_file_name)
            self.save_multifigures_report(figures, report_path)

    def process(self) -> None:
        with StageReporter("Scalar IPC dynamic report generation"):
            self._process()
        final_logger.add_general_info(f"Scalar IPC dynamic reports: {self.output_dir}")
