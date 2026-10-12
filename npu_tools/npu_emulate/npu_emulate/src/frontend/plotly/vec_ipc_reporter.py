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


class VecIPCReporter(PlotlyReporterBase):
    @staticmethod
    def get_vf_ipc_points(
        core_id: int,
        core_type_id: int,
    ) -> Tuple[List[int], List[float], List[float], List[float]]:
        rows = database_handler.execute(
            """
            SELECT
                VF.ScalarIPC,
                VF.ExecIPC,
                VF.LdStIPC
            FROM VfIPC AS VF
            LEFT JOIN ExecutedInstructions AS EI
                ON EI.ExecInstrId = VF.ExecInstrId
            WHERE
                EI.ExecInstrCoreId = :core_id
                AND EI.CoreTypeId = :core_type_id
            ORDER BY EI.ExecInstrTickStart
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchall()
        return (
            [(i + 1) for i in range(len(rows))],
            [row["ScalarIPC"] for row in rows],
            [row["ExecIPC"] for row in rows],
            [row["LdStIPC"] for row in rows],
        )

    @staticmethod
    def get_vf_simt_ipc_points(
        core_id: int,
        core_type_id: int,
    ) -> Tuple[List[int], List[float], List[float], List[float]]:
        rows = database_handler.execute(
            """
            SELECT
                VFS.ExecIPC,
                VFS.LdStIPC,
                VFS.BranchIPC
            FROM VfSimtIPC AS VFS
            LEFT JOIN ExecutedInstructions AS EI
                ON EI.ExecInstrId = VFS.ExecInstrId
            WHERE
                EI.ExecInstrCoreId = :core_id
                AND EI.CoreTypeId = :core_type_id
            ORDER BY EI.ExecInstrTickStart
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchall()
        return (
            [(i + 1) for i in range(len(rows))],
            [row["ExecIPC"] for row in rows],
            [row["LdStIPC"] for row in rows],
            [row["BranchIPC"] for row in rows],
        )

    def _process_vf_instrs(self, core_id: int, core_types: List[int]) -> None:
        figures: List[go.Figure] = []

        for core_type_id in core_types:
            # Skip non-vector subcores
            if core_type_id not in [CoreType.AIV0.value, CoreType.AIV1.value]:
                continue

            numbers, ipc_values_scalar, ipc_values_exec, ipc_values_ldst = (
                self.get_vf_ipc_points(core_id, core_type_id)
            )

            if not numbers:
                continue

            fig = go.Figure()
            fig.add_trace(
                go.Scatter(
                    x=numbers, y=ipc_values_scalar, mode="lines+markers", name="Scalar"
                )
            )
            fig.add_trace(
                go.Scatter(
                    x=numbers, y=ipc_values_exec, mode="lines+markers", name="Execution"
                )
            )
            fig.add_trace(
                go.Scatter(
                    x=numbers,
                    y=ipc_values_ldst,
                    mode="lines+markers",
                    name="Load/Store",
                )
            )

            title = f"IPC per VF ({CoreType(core_type_id).name})"
            fig.update_layout(
                title=title,
                xaxis_title="VF Number",
                yaxis_title="Value",
                width=None,
            )

            figures.append(fig)

        if not figures:
            return

        report_file_name = f"core_{core_id}_vf_ipc_report.html"
        report_path = os.path.join(self.output_dir, report_file_name)
        self.save_multifigures_report(figures, report_path)

    def _process_vf_simt_instrs(self, core_id: int, core_types: List[int]) -> None:
        figures: List[go.Figure] = []

        for core_type_id in core_types:
            # Skip non-vector subcores
            if core_type_id not in [CoreType.AIV0.value, CoreType.AIV1.value]:
                continue

            numbers, ipc_values_exec, ipc_values_ldst, ipc_values_branch = (
                self.get_vf_simt_ipc_points(core_id, core_type_id)
            )

            if not numbers:
                continue

            fig = go.Figure()
            fig.add_trace(
                go.Scatter(
                    x=numbers, y=ipc_values_branch, mode="lines+markers", name="Branch"
                )
            )
            fig.add_trace(
                go.Scatter(
                    x=numbers, y=ipc_values_exec, mode="lines+markers", name="Execution"
                )
            )
            fig.add_trace(
                go.Scatter(
                    x=numbers,
                    y=ipc_values_ldst,
                    mode="lines+markers",
                    name="Load/Store",
                )
            )

            title = (
                f"IPC per VF_SIMT ({CoreType(core_type_id).name}), "
                f"window size = {int(biprof_ctx.sliding_window_vec)}"
            )
            fig.update_layout(
                title=title,
                xaxis_title="VF_SIMT Number (Time)",
                yaxis_title="Value",
                width=None,
            )

            figures.append(fig)

        if not figures:
            return

        report_file_name = f"core_{core_id}_vf_simt_ipc_report.html"
        report_path = os.path.join(self.output_dir, report_file_name)
        self.save_multifigures_report(figures, report_path)

    def _process(self) -> None:
        cores = BackendDBManager.get_active_cores()
        core_types = BackendDBManager.get_active_core_types_ordered()

        for core_id in cores:
            self._process_vf_instrs(core_id, core_types)
            self._process_vf_simt_instrs(core_id, core_types)

    def process(self) -> None:
        with StageReporter("Vector IPC report generation"):
            self._process()
        final_logger.add_general_info(f"Vector IPC reports: {self.output_dir}")
