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


class VecIPCDynamicReporter(PlotlyReporterBase):
    @staticmethod
    def get_vf_ipc_dynamic_points(
        core_id: int,
        core_type_id: int,
    ) -> Tuple[List[int], List[float], List[float], List[float]]:
        indices: List[int] = []
        scalar: List[float] = []
        exec_: List[float] = []
        ldst: List[float] = []

        for i, row in enumerate(
            database_handler.iter_rows(
                """
            SELECT
                ScalarIPC,
                ExecIPC,
                LdStIPC
            FROM VfIPCDynamicView
            WHERE CoreId = :core_id
            AND CoreTypeId = :core_type_id
        """,
                {"core_id": core_id, "core_type_id": core_type_id},
            )
        ):
            indices.append(i)
            scalar.append(row["ScalarIPC"])
            exec_.append(row["ExecIPC"])
            ldst.append(row["LdStIPC"])

        return indices, scalar, exec_, ldst

    @staticmethod
    def get_vf_simt_ipc_dynamic_points(
        core_id: int,
        core_type_id: int,
    ) -> Tuple[List[int], List[float], List[float], List[float]]:
        indices: List[int] = []
        exec_: List[float] = []
        ldst: List[float] = []
        branch: List[float] = []

        for i, row in enumerate(
            database_handler.iter_rows(
                """
            SELECT
                ExecIPC,
                LdStIPC,
                BranchIPC
            FROM VfSimtIPCDynamicView
            WHERE CoreId = :core_id
            AND CoreTypeId = :core_type_id
        """,
                {"core_id": core_id, "core_type_id": core_type_id},
            )
        ):
            indices.append(i)
            exec_.append(row["ExecIPC"])
            ldst.append(row["LdStIPC"])
            branch.append(row["BranchIPC"])

        return indices, exec_, ldst, branch

    def _process_vf_instrs(self, core_id: int, core_types: List[int]) -> None:
        figures: List[go.Figure] = []

        for core_type_id in core_types:
            # Skip non-vector subcores
            if core_type_id not in [CoreType.AIV0.value, CoreType.AIV1.value]:
                continue

            window_numbers, ipc_values_scalar, ipc_values_exec, ipc_values_ldst = (
                self.get_vf_ipc_dynamic_points(core_id, core_type_id)
            )

            if not window_numbers:
                continue

            fig = go.Figure()
            fig.add_trace(
                go.Scatter(
                    x=window_numbers, y=ipc_values_scalar, mode="lines", name="Scalar"
                )
            )
            fig.add_trace(
                go.Scatter(
                    x=window_numbers, y=ipc_values_exec, mode="lines", name="Execution"
                )
            )
            fig.add_trace(
                go.Scatter(
                    x=window_numbers,
                    y=ipc_values_ldst,
                    mode="lines",
                    name="Load/Store",
                )
            )

            title = (
                f"VF IPC Dynamic ({CoreType(core_type_id).name}), "
                f"window size = {int(biprof_ctx.sliding_window_vec)}"
            )
            fig.update_layout(
                title=title,
                xaxis_title="Window Number",
                yaxis_title="Value",
                width=None,
            )

            figures.append(fig)

        if not figures:
            return

        report_file_name = f"core_{core_id}_vf_ipc_dynamic_report.html"
        report_path = os.path.join(self.output_dir, report_file_name)
        self.save_multifigures_report(figures, report_path)

    def _process_vf_simt_instrs(self, core_id: int, core_types: List[int]) -> None:
        figures: List[go.Figure] = []

        for core_type_id in core_types:
            # Skip non-vector subcores
            if core_type_id not in [CoreType.AIV0.value, CoreType.AIV1.value]:
                continue

            window_numbers, ipc_values_exec, ipc_values_ldst, ipc_values_branch = (
                self.get_vf_simt_ipc_dynamic_points(core_id, core_type_id)
            )

            if not window_numbers:
                continue

            fig = go.Figure()
            fig.add_trace(
                go.Scatter(
                    x=window_numbers, y=ipc_values_branch, mode="lines", name="Branch"
                )
            )
            fig.add_trace(
                go.Scatter(
                    x=window_numbers, y=ipc_values_exec, mode="lines", name="Execution"
                )
            )
            fig.add_trace(
                go.Scatter(
                    x=window_numbers, y=ipc_values_ldst, mode="lines", name="Load/Store"
                )
            )

            title = (
                f"VF_SIMT IPC Dynamic ({CoreType(core_type_id).name}), "
                f"window size = {int(biprof_ctx.sliding_window_vec)}"
            )
            fig.update_layout(
                title=title,
                xaxis_title="Window Number",
                yaxis_title="Value",
                width=None,
            )

            figures.append(fig)

        if not figures:
            return

        report_file_name = f"core_{core_id}_vf_simt_ipc_dynamic_report.html"
        report_path = os.path.join(self.output_dir, report_file_name)
        self.save_multifigures_report(figures, report_path)

    def _process(self) -> None:
        cores = BackendDBManager.get_active_cores()
        core_types = BackendDBManager.get_active_core_types_ordered()

        for core_id in cores:
            self._process_vf_instrs(core_id, core_types)
            self._process_vf_simt_instrs(core_id, core_types)

    def process(self) -> None:
        with StageReporter("Vector IPC dynamic report generation"):
            self._process()
        final_logger.add_general_info(f"Vector IPC dynamic reports: {self.output_dir}")
