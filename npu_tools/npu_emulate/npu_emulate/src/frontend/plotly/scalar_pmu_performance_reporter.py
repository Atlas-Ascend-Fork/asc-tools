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
from plotly.subplots import make_subplots  # type: ignore
import plotly.express as px  # type: ignore
from typing import List, Dict, Any
from npu_emulate.src.common.biprof_enum import CoreType
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.frontend.base.plotly_reporter_base import PlotlyReporterBase


class ScalarPMUPerformanceReporter(PlotlyReporterBase):
    INSTR_KEYS = [
        "issue_exe_instr_num",
        "issue_ldst_instr_num",
        "issue_jump_instr_num",
        "issue_variable_instr_num",
        "issue_float_instr_num",
        "issue_pipe_instr_num",
        "issue_others_instr_num",
    ]
    INSTR_LABELS = [
        "EXE",
        "LDST",
        "Jump",
        "Variable",
        "Float",
        "Pipe",
        "Others",
    ]
    COLORS = px.colors.qualitative.Set3

    @staticmethod
    def get_stats(core_id: int, core_type_id: int) -> Dict[str, int]:
        rows = database_handler.execute(
            """
        SELECT CounterName, CounterValue FROM PMUScalarCounters
        WHERE CoreId = :core_id
        AND CoreTypeId = :core_type_id
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchall()
        res = {row["CounterName"]: row["CounterValue"] for row in rows}
        return res

    @staticmethod
    def calculate_metrics(stats: Dict[str, int]) -> Dict[str, Any]:
        metrics: Dict[str, Any] = {}
        try:
            metrics["misprediction_rate"] = (
                stats["branch_instr_miss_counter"]
                / stats["branch_instr_predict_counter"]
            ) * 100
        except ZeroDivisionError:
            metrics["misprediction_rate"] = 0.0
        try:
            metrics["indirect_jump_ratio"] = (
                stats["issue_branch_reg_instr_num"]
                / stats["issue_branch_imm_instr_num"]
            ) * 100
        except ZeroDivisionError:
            metrics["indirect_jump_ratio"] = 0.0
        try:
            metrics["branch_stall_ratio"] = (
                stats["issue_branch_stall_cycle"] / stats["kernel_total_ticks"]
            ) * 100
        except ZeroDivisionError:
            metrics["branch_stall_ratio"] = 0.0
        try:
            metrics["icache_miss_ratio"] = (
                stats["issue_icache_miss_cycle"] / stats["kernel_total_ticks"]
            ) * 100
        except ZeroDivisionError:
            metrics["icache_miss_ratio"] = 0.0
        try:
            metrics["bar_all_stall_ratio"] = (
                stats["issue_bar_all_stall_cycle"] / stats["kernel_total_ticks"]
            ) * 100
        except ZeroDivisionError:
            metrics["bar_all_stall_ratio"] = 0.0
        try:
            metrics["ld_hazard_ratio"] = (
                (stats["issue_ld_hazard_cycle"] + stats["issue_ld_miss_hazard_cycle"])
                / stats["kernel_total_ticks"]
            ) * 100
        except ZeroDivisionError:
            metrics["ld_hazard_ratio"] = 0.0
        try:
            metrics["st_hazard_ratio"] = (
                stats["issue_st_hazard_cycle"] / stats["kernel_total_ticks"]
            ) * 100
        except ZeroDivisionError:
            metrics["st_hazard_ratio"] = 0.0
        try:
            metrics["stb_pressure_ratio"] = (
                (stats["issue_lsu_stb_full_cycle"] + stats["issue_lsu_stb_drain_cycle"])
                / stats["kernel_total_ticks"]
            ) * 100
        except ZeroDivisionError:
            metrics["stb_pressure_ratio"] = 0.0

        instr_num_keys = [
            "issue_exe_instr_num",
            "issue_ldst_instr_num",
            "issue_jump_instr_num",
            "issue_variable_instr_num",
            "issue_float_instr_num",
            "issue_pipe_instr_num",
            "issue_others_instr_num",
        ]
        instr_num_sum = sum(stats[k] for k in instr_num_keys)
        try:
            metrics["instr_num_ratios"] = {
                k: (stats[k] / instr_num_sum) * 100 for k in instr_num_keys
            }
        except ZeroDivisionError:
            metrics["instr_num_ratios"] = {k: 0.0 for k in instr_num_keys}
        metrics["instr_num_sum"] = instr_num_sum

        try:
            metrics["su_utilization"] = (
                stats["busy_cycle"] / stats["kernel_total_ticks"]
            ) * 100
        except ZeroDivisionError:
            metrics["su_utilization"] = 0.0
        try:
            metrics["ldst_exe_ratio"] = (
                stats["issue_ldst_instr_num"] / stats["issue_exe_instr_num"]
            ) * 100
        except ZeroDivisionError:
            metrics["ldst_exe_ratio"] = 0.0
        try:
            metrics["issue_ratio"] = (
                stats["single_issue_cycle"] / stats["double_issue_cycle"]
            ) * 100
        except ZeroDivisionError:
            metrics["issue_ratio"] = 0.0

        payloadq_stall_keys = [
            "issue_mte1_payloadQ_stall_cycle",
            "issue_mte2_payloadQ_stall_cycle",
            "issue_mte3_payloadQ_stall_cycle",
            "issue_fixp_payloadQ_stall_cycle",
            "issue_cube_payloadQ_stall_cycle",
            "issue_vec_payloadQ_stall_cycle",
        ]
        payloadq_stall_labels = [
            "MTE1",
            "MTE2",
            "MTE3",
            "FIXP",
            "CUBE",
            "VEC",
        ]
        try:
            metrics["payloadq_stall_ratios"] = {
                k: (stats[k] / stats["kernel_total_ticks"]) * 100
                for k in payloadq_stall_keys
            }
        except ZeroDivisionError:
            metrics["payloadq_stall_ratios"] = {k: 0.0 for k in payloadq_stall_keys}
        metrics["payloadq_stall_labels"] = payloadq_stall_labels

        try:
            metrics["dcci_ratio"] = (
                stats["end_dcci_busy_cycle"] / stats["kernel_total_ticks"]
            ) * 100
        except ZeroDivisionError:
            metrics["dcci_ratio"] = 0.0

        return metrics

    @staticmethod
    def get_hist(
        stats: Dict[str, int], metrics: Dict[str, Any], core_type_id: int
    ) -> go.Figure:
        categories = []
        values = []
        categories.extend(
            [
                "SU Utilization (%)",
                "Misprediction Ratio (%)",
                "Indirect Jump Ratio (%)",
                "Branch Stall (%)",
                "Icache Miss Ratio (%)",
                "Bar All Stall Ratio (%)",
                "LD Hazard Ratio (%)",
                "ST Hazard Ratio (%)",
                "STB Pressure Ratio (%)",
                "VEC Queue Stall Ratio (%)",
                "CUBE Queue Stall Ratio (%)",
                "FIXP Queue Stall Ratio (%)",
                "MTE3 Queue Stall Ratio (%)",
                "MTE2 Queue Stall Ratio (%)",
                "MTE1 Queue Stall Ratio (%)",
                "DCCI Busy Ratio (%)",
                "LDST/Exe Ratio (%)",
                "Single/Double Issue Ratio (%)",
            ]
        )
        values.extend(
            [
                metrics["su_utilization"],
                metrics["misprediction_rate"],
                metrics["indirect_jump_ratio"],
                metrics["branch_stall_ratio"],
                metrics["icache_miss_ratio"],
                metrics["bar_all_stall_ratio"],
                metrics["ld_hazard_ratio"],
                metrics["st_hazard_ratio"],
                metrics["stb_pressure_ratio"],
                metrics["payloadq_stall_ratios"]["issue_vec_payloadQ_stall_cycle"],
                metrics["payloadq_stall_ratios"]["issue_cube_payloadQ_stall_cycle"],
                metrics["payloadq_stall_ratios"]["issue_fixp_payloadQ_stall_cycle"],
                metrics["payloadq_stall_ratios"]["issue_mte3_payloadQ_stall_cycle"],
                metrics["payloadq_stall_ratios"]["issue_mte2_payloadQ_stall_cycle"],
                metrics["payloadq_stall_ratios"]["issue_mte1_payloadQ_stall_cycle"],
                metrics["dcci_ratio"],
                metrics["ldst_exe_ratio"],
                metrics["issue_ratio"],
            ]
        )
        categories.append("Dcache Allocated (KB)")
        values.append(stats["dcache_allocate_size"] / 1024)

        categories.reverse()
        values.reverse()

        fig = go.Figure()

        fig.add_trace(
            go.Bar(
                x=values,
                y=categories,
                orientation="h",
                marker=dict(
                    color=values,
                    colorscale="Inferno",
                    showscale=True,
                    colorbar=dict(title="Value"),
                ),
                text=[f"{v:.2f}" for v in values],
                textposition="outside",
                hovertemplate="<b>%{y}</b><br>Value: %{x:.2f}<extra></extra>",
            )
        )
        fig.update_layout(
            title={
                "text": "Performance Summary"
                + f" - {CoreType(core_type_id).name}<br>"
                + f"<sup>kernel_total_ticks: {stats['kernel_total_ticks']:,} "
                + f"| system_total_ticks: {stats['system_total_ticks']:,}</sup>",
                "x": 0.5,
                "xanchor": "center",
                "font": dict(size=18),
            },
            xaxis_title="Value",
            yaxis_title="Metrics",
            height=700,
            width=1200,
            font=dict(family="Arial, sans-serif", size=12),
            plot_bgcolor="rgba(240, 240, 240, 0.8)",
            paper_bgcolor="white",
            showlegend=False,
            xaxis=dict(gridcolor="lightgray", linecolor="black"),
            yaxis=dict(gridcolor="lightgray", linecolor="black"),
            margin=dict(l=200, r=100, t=120, b=70),
        )
        return fig

    @classmethod
    def get_pie(cls, stats: Dict[str, int]) -> go.Pie:
        values = [stats[k] for k in cls.INSTR_KEYS]
        return go.Pie(
            labels=cls.INSTR_LABELS,
            values=values,
            textinfo="label+percent",
            hoverinfo="label+percent+value",
            marker=dict(colors=cls.COLORS, line=dict(color="white", width=2)),
            textposition="inside",
            hole=0.3,
        )

    def _process(self) -> None:
        for core_id in BackendDBManager.get_active_cores():
            figures: List[go.Figure] = []
            core_type_ids = BackendDBManager.get_active_core_types_ordered()
            pie_fig = make_subplots(
                rows=1,
                cols=3,
                subplot_titles=[
                    f"{CoreType(core_type_id).name}" for core_type_id in core_type_ids
                ],
                specs=[[{"type": "pie"}, {"type": "pie"}, {"type": "pie"}]],
            )
            for i, core_type_id in enumerate(core_type_ids):
                stats: Dict[str, int] = self.get_stats(core_id, core_type_id)
                metrics = self.calculate_metrics(stats)
                fig = self.get_hist(stats, metrics, core_type_id)
                figures.append(fig)

                pie = self.get_pie(stats)
                pie_fig.add_trace(pie, row=1, col=i + 1)

            pie_fig.update_layout(
                title={
                    "text": "Instruction Count Breakdown",
                    "x": 0.5,
                    "xanchor": "center",
                    "font": dict(size=18),
                },
                height=500,
                width=1400,
                margin=dict(t=120, b=100),
                showlegend=True,
                legend=dict(
                    orientation="h", yanchor="bottom", y=-0.15, xanchor="center", x=0.5
                ),
            )

            figures.append(pie_fig)

            if not figures:
                continue

            report_file_name = f"pmu_core_{core_id}_scalar_performance_report.html"
            report_path = os.path.join(self.output_dir, report_file_name)
            self.save_multifigures_report(
                figures,
                report_path,
                "AICore Scalar PMU Performance Analysis",
            )

    def process(self) -> None:
        with StageReporter("[PMU] Scalar performance report generation"):
            self._process()
        final_logger.add_general_info(
            f"[PMU] Scalar performance reports: {self.output_dir}"
        )
