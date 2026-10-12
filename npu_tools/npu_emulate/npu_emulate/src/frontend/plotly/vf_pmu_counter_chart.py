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

"""Per-VF charts of counters the analyzer already produced.

No arithmetic lives here: every trace is a value read straight from a table.
Most groups plot synthetic counters (source="synth", the default); a group
marked source="raw" plots raw counters instead, for quantities that are
genuinely raw and not persisted as synth (e.g. blocked cycles).

The import-time check below rejects a synth group that asks for a
subcore-only metric, since only VF-scoped synthetics are persisted per VF
and it would otherwise render as an empty trace.
"""

import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import plotly.graph_objects as go  # type: ignore

from npu_emulate.src.backend.analyzers.vf_pmu import storage
from npu_emulate.src.backend.analyzers.vf_pmu.metrics import (
    IDU_WIDTH,
    SYN_PREFIX,
    VF_DISPLAY_NAMES,
    display_name,
)
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.common.biprof_enum import CoreType
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.frontend.base.plotly_reporter_base import (
    PlotlyReporterBase,
)

_PALETTE = [
    "#1f77b4",
    "#ff7f0e",
    "#2ca02c",
    "#d62728",
    "#9467bd",
    "#8c564b",
    "#e377c2",
    "#7f7f7f",
    "#bcbd22",
    "#17becf",
    "#aec7e8",
    "#ffbb78",
    "#98df8a",
    "#ff9896",
    "#c5b0d5",
    "#d62728",
    "#e377c2",
    "#bcbd22",
    "#17becf",
    "#9467bd",
    "#1f77b4",
    "#ff7f0e",
    "#2ca02c",
    "#8c564b",
    "#7f7f7f",
    "#c5b0d5",
    "#ffbb78",
    "#98df8a",
    "#ff9896",
    "#aec7e8",
    "#d62728",
    "#9467bd",
    "#1f77b4",
    "#ff7f0e",
    "#2ca02c",
    "#d62728",
    "#9467bd",
    "#8c564b",
    "#e377c2",
    "#7f7f7f",
    "#bcbd22",
    "#17becf",
    "#aec7e8",
    "#ffbb78",
    "#98df8a",
    "#ff9896",
    "#c5b0d5",
    "#d62728",
    "#e377c2",
    "#bcbd22",
    "#17becf",
    "#9467bd",
    "#1f77b4",
    "#ff7f0e",
    "#2ca02c",
    "#8c564b",
    "#7f7f7f",
    "#c5b0d5",
    "#ffbb78",
    "#98df8a",
    "#ff9896",
    "#aec7e8",
    "#d62728",
    "#9467bd",
]


@dataclass
class MetricGroup:
    """One figure: a trace per metric, read straight out of the DB.

    `source` picks the data: "synth" -> `metrics` are DISPLAY names read from
    the per-VF synthetic table; "raw" -> `metrics` are raw counter names (c_*)
    read from the per-VF delta view (for quantities that aren't derived, e.g.
    blocked cycles); "vf_kernel" -> `metrics` are the computed keys from
    `_fetch_vf_kernel_metrics` (cycles/ipc_sim/efficiency - the same numbers
    the Perf Summary per VF report shows, which live in neither of the other
    two tables). `labels` renames a trace; its keys match `metrics`.

    `secondary_metrics` plots those metrics (a subset of `metrics`) against a
    right-hand y-axis titled `secondary_yaxis_title`, for a group whose
    series don't share a scale (e.g. cycle counts vs. a 0-6 IPC value).

    `xaxis_range`/`yaxis_range`/`secondary_yaxis_range` each pin that axis to
    a fixed [min, max] instead of Plotly auto-scaling to the data. `None`
    (the default) leaves that axis auto-scaled.
    """

    name: str
    title: str
    metrics: List[str]
    yaxis_title: str
    labels: Dict[str, str] = field(default_factory=dict)
    core_type_filter: Optional[List[int]] = None  # None = all core types
    source: str = "synth"  # "synth" | "raw" | "vf_kernel"
    secondary_metrics: List[str] = field(default_factory=list)
    secondary_yaxis_title: str = ""
    xaxis_range: Optional[Tuple[float, float]] = None
    yaxis_range: Optional[Tuple[float, float]] = None
    secondary_yaxis_range: Optional[Tuple[float, float]] = None

    def label_of(self, metric: str) -> str:
        return self.labels.get(metric, metric.replace("_", " ").title())


_VEC = [CoreType.AIV0.value, CoreType.AIV1.value]


def _same_name(name: str) -> str:
    return name


def _fetch_vf_kernel_metrics(
    core_id: int, core_type_id: int
) -> Dict[int, Dict[str, float]]:
    """{vf_id: {cycles, ipc_sim, efficiency, efficiency_pct}}.

    Matches the Perf Summary per VF report's own numbers exactly: Cycles is
    VfKernelTicks (not c_cycle_rvec_busy), IPC (sim) is dispatched slots
    over those kernel ticks (not the rvec_busy-normalized syn_c_ipc_total -
    see PMU_Metrics_explanation.md), and Efficiency is IPC (sim) / IDU_WIDTH.
    efficiency_pct is that same 0-1 fraction x 100, a display-only sibling
    (same pattern as the *_pct custom metrics in metrics.py). None of these
    are persisted anywhere, so this recomputes them the same way
    vf_pmu_perf_per_vf_reporter._compute_ipc() does.
    """
    ticks_map = storage.fetch_vf_kernel_ticks(core_id, core_type_id)
    deltas = storage.fetch_vf_deltas(core_id, core_type_id, ["c_slot_idu_disp_total"])
    result: Dict[int, Dict[str, float]] = {}
    for vf_id, ticks in ticks_map.items():
        cycles = max(ticks, 1)
        total = deltas.get(vf_id, {}).get("c_slot_idu_disp_total", 0)
        ipc_sim = total / cycles
        efficiency = ipc_sim / IDU_WIDTH
        result[vf_id] = {
            "cycles": float(cycles),
            "ipc_sim": ipc_sim,
            "efficiency": efficiency,
            "efficiency_pct": efficiency * 100,
        }
    return result


class VfPMUCounterChart(PlotlyReporterBase):
    """One HTML per (core, metric group); one figure per core type."""

    METRIC_GROUPS: List[MetricGroup] = [
        # Synthetic counter based charts
        MetricGroup(
            name="ipc",
            title="SIMD IPC",
            metrics=[
                "ipc_ex",
                "ipc_ld",
                "ipc_st",
                "ipc_ga",
                "ipc_sc",
                "ipc_su",
                "ipc_lp",
                "ipc_total",
            ],
            labels={
                "ipc_ex": "EX (Compute)",
                "ipc_ld": "LD (Load)",
                "ipc_st": "ST (Store)",
                "ipc_ga": "GA (Gather)",
                "ipc_sc": "SC (Scatter)",
                "ipc_su": "SU (Scalar)",
                "ipc_lp": "LP (Loop)",
                "ipc_total": "Total",
            },
            yaxis_title="Instructions per cycle",
            core_type_filter=_VEC,
        ),
        MetricGroup(
            name="load_store_efficiency",
            title="Load/Store Efficiency",
            metrics=["ub_bw_ldu", "ub_bw_stu", "ub_bw_lsu", "ub_bw_gsu"],
            labels={
                "ub_bw_ldu": "LDU",
                "ub_bw_stu": "STU",
                "ub_bw_lsu": "LSU (LD+ST)",
                "ub_bw_gsu": "GSU",
            },
            yaxis_title="Bytes per cycle",
        ),
        MetricGroup(
            name="instr_perf",
            title="SIMD Instruction Performance",
            metrics=[
                "retired_pct",
                "frontend_bound_pct",
                "backend_vec_bound_pct",
                "backend_asu_bound_pct",
            ],
            labels={
                "retired_pct": "Retired",
                "frontend_bound_pct": "Frontend bound",
                "backend_vec_bound_pct": "Backend VEC bound",
                "backend_asu_bound_pct": "Backend ASU bound",
            },
            yaxis_title="Percentage (%)",
            yaxis_range=(0, 100),
        ),
        MetricGroup(
            name="vf_duration_efficiency",
            title="Duration & Efficiency",
            metrics=["cycles", "efficiency_pct"],
            labels={"cycles": "Duration", "efficiency_pct": "Efficiency"},
            yaxis_title="Efficiency (%)",
            yaxis_range=(0, 100),
            core_type_filter=_VEC,
            source="vf_kernel",
            secondary_metrics=["cycles"],
            secondary_yaxis_title="Cycles",
        ),
        # Raw counter based charts
        MetricGroup(
            name="exq_stalls",
            title="Execution Queue Stalls",
            metrics=[
                "c_cycle_exq0_src_not_ready_all",
                "c_cycle_exq1_src_not_ready_all",
            ],
            labels={
                "c_cycle_exq0_src_not_ready_all": "EXQ0 Stall (src not ready)",
                "c_cycle_exq1_src_not_ready_all": "EXQ1 Stall (src not ready)",
            },
            yaxis_title="Cycles",
            source="raw",
            core_type_filter=_VEC,
        ),
        MetricGroup(
            name="lsu_grants",
            title="Load/Store Grant Sizes",
            metrics=["c_stat_ldu_grant_size", "c_stat_stu_grant_size"],
            labels={
                "c_stat_ldu_grant_size": "LDU grant size",
                "c_stat_stu_grant_size": "STU grant size",
            },
            yaxis_title="Bytes",
            source="raw",
            core_type_filter=_VEC,
        ),
        MetricGroup(
            name="exq_empty",
            title="Execution Queue Empty",
            metrics=["c_cycle_idu_not_disp.fe_bound", "c_exq0_empty", "c_exq1_empty"],
            labels={
                "c_cycle_idu_not_disp.fe_bound": "IDU no dispatch (FE bound)",
                "c_exq0_empty": "EXQ0 empty",
                "c_exq1_empty": "EXQ1 empty",
            },
            yaxis_title="Cycles",
            source="raw",
            core_type_filter=_VEC,
        ),
    ]

    @classmethod
    def get_group_names(cls) -> List[str]:
        return [group.name for group in cls.METRIC_GROUPS]

    def _figure(
        self, group: MetricGroup, core_id: int, core_type_id: int
    ) -> Optional[go.Figure]:
        # Raw, synthetic and vf_kernel live in different places; a group
        # reads one. All key the series by the names in group.metrics, so
        # the loop below is source-agnostic.
        if group.source == "raw":
            by_vf = storage.fetch_vf_deltas(core_id, core_type_id, list(group.metrics))
            key_of = _same_name  # raw names, as configured
        elif group.source == "vf_kernel":
            by_vf = _fetch_vf_kernel_metrics(core_id, core_type_id)
            key_of = _same_name  # computed names, as configured
        else:
            storage_names = [SYN_PREFIX + name for name in group.metrics]
            by_vf = storage.fetch_vf_synth_values(core_id, core_type_id, storage_names)
            key_of = display_name  # strip the syn_c_ prefix
        if not by_vf:
            return None

        # {series_name: {vf_id: value}}, so a metric missing for one VF just
        # leaves a gap in that trace instead of shifting the series.
        series: Dict[str, Dict[int, float]] = {}
        for vf_id, values in by_vf.items():
            for name, value in values.items():
                series.setdefault(key_of(name), {})[vf_id] = value

        fig = go.Figure()
        for idx, metric in enumerate(group.metrics):
            points = series.get(metric)
            if not points:
                continue
            vf_ids = sorted(points)
            label = group.label_of(metric)
            color = _PALETTE[idx % len(_PALETTE)]
            y_vals = [points[vf] for vf in vf_ids]
            on_secondary = metric in group.secondary_metrics
            axis_label = (
                group.secondary_yaxis_title if on_secondary else group.yaxis_title
            )
            fig.add_trace(
                go.Scatter(
                    x=vf_ids,
                    y=y_vals,
                    mode="lines",
                    name=label,
                    line={"width": 1, "color": color},
                    legendgroup=label,
                    yaxis="y2" if on_secondary else "y",
                    hovertemplate=(
                        f"<b>{label}</b><br>"
                        f"VF: %{{x}}<br>"
                        f"{axis_label}: %{{y:.4f}}"
                        f"<extra></extra>"
                    ),
                )
            )

        absent = [m for m in group.metrics if m not in series]
        if absent:
            final_logger.add_general_info(
                f"[PMU] {group.name} (core {core_id}, "
                f"{CoreType(core_type_id).name}): no per-VF rows for "
                f"{absent}; has the analyzer run?"
            )

        xaxis_cfg: Dict[str, object] = {"title": "VF ID"}
        if group.xaxis_range is not None:
            xaxis_cfg["range"] = list(group.xaxis_range)
        yaxis_cfg: Dict[str, object] = {"title": group.yaxis_title}
        if group.yaxis_range is not None:
            yaxis_cfg["range"] = list(group.yaxis_range)

        layout_kwargs = {
            "title": {
                "text": (f"{group.title} per VF ({CoreType(core_type_id).name})"),
                "x": 0.5,
                "font": {"size": 18},
            },
            "xaxis": xaxis_cfg,
            "yaxis": yaxis_cfg,
            "height": 600,
            "uirevision": "fixed",
            "template": "plotly_white",
            "hoverlabel": {"font_size": 12, "font_family": "Helvetica"},
            "legend": {
                "orientation": "v",
                "xanchor": "left",
                "x": 1.02,
                "yanchor": "top",
                "y": 1,
                "font": {"size": 9},
            },
        }
        if group.secondary_metrics:
            yaxis2_cfg: Dict[str, object] = {
                "title": group.secondary_yaxis_title,
                "overlaying": "y",
                "side": "right",
            }
            if group.secondary_yaxis_range is not None:
                yaxis2_cfg["range"] = list(group.secondary_yaxis_range)
            layout_kwargs["yaxis2"] = yaxis2_cfg
        fig.update_layout(**layout_kwargs)
        return fig

    def _process_group(
        self, group: MetricGroup, core_id: int, core_types: List[int]
    ) -> None:
        figures: List[go.Figure] = []
        for core_type_id in core_types:
            if (
                group.core_type_filter is not None
                and core_type_id not in group.core_type_filter
            ):
                continue
            fig = self._figure(group, core_id, core_type_id)
            if fig is not None:
                figures.append(fig)

        if not figures:
            return

        report_file_name = f"pmu_counter_chart_core_{core_id}_{group.name}.html"
        report_path = os.path.join(self.output_dir, report_file_name)
        self.save_multifigures_report(figures, report_path)

    def _enabled_group_names(self) -> List[str]:
        """Which groups to generate.

        None means the CLI option was never parsed for this run mode; fall
        back to every group rather than silently producing nothing. An empty
        list is an explicit opt-out and is honored.
        """
        enabled = getattr(biprof_ctx, "vec_pmu_reports", None)
        if enabled is None:
            return self.get_group_names()

        enabled = list(enabled)
        unknown = set(enabled) - set(self.get_group_names())
        if unknown:
            final_logger.add_general_info(
                f"[PMU] unknown counter groups ignored: {sorted(unknown)}"
            )
        return enabled

    def _process(self) -> None:
        cores = BackendDBManager.get_active_cores()
        core_types = BackendDBManager.get_active_core_types_ordered()
        enabled = self._enabled_group_names()

        for core_id in cores:
            for group in self.METRIC_GROUPS:
                if group.name in enabled:
                    self._process_group(group, core_id, core_types)

    def process(self) -> None:
        with StageReporter("[PMU] VF counter charts generation"):
            self._process()
        final_logger.add_general_info(f"[PMU] VF counter charts: {self.output_dir}")


def _check_groups(groups: List[MetricGroup]) -> None:
    """Catch, at import, a group that would render an empty trace.

    A metric the analyzer does not persist per VF (scope SUBCORE, or simply
    misspelled) has no rows in VfPMUSynthValuesPerVf.
    """
    for group in groups:
        # Raw groups name raw counters (c_*), which are not in the synthetic
        # namespace, so only synth groups are validated against it.
        if group.source == "synth":
            unknown = set(group.metrics) - VF_DISPLAY_NAMES
            assert not unknown, (
                f"{group.name}: not persisted per VF (check MetricScope): "
                f"{sorted(unknown)}"
            )
        unlabeled = set(group.labels) - set(group.metrics)
        assert not unlabeled, (
            f"{group.name}: labels for metrics not in the group: {sorted(unlabeled)}"
        )
        assert len(set(group.metrics)) == len(group.metrics), (
            f"{group.name}: duplicate metric in the group"
        )


_check_groups(VfPMUCounterChart.METRIC_GROUPS)
