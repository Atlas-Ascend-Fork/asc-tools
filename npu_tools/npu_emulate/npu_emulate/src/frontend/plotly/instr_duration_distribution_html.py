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

import sqlite3
import math
from collections import defaultdict
from typing import Dict, List, Tuple, Iterator

import plotly.graph_objects as go  # type:ignore
import plotly.io as pio  # type:ignore

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_constants import SYNCHRO_INSTRS
from npu_emulate.src.common.biprof_enum import InstrType
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.frontend.base.plotly_reporter_base import PlotlyReporterBase
from npu_emulate.src.common.utils import decompose_instr_id


class RunningStats:
    __slots__ = ("n", "mean", "_m2", "min", "max")

    def __init__(self) -> None:
        self.n = 0
        self.mean = 0.0
        self._m2 = 0.0
        self.min = math.inf
        self.max = -math.inf

    def add(self, x: float) -> None:
        self.n += 1
        delta = x - self.mean
        self.mean += delta / self.n
        self._m2 += delta * (x - self.mean)
        if x < self.min:
            self.min = x
        if x > self.max:
            self.max = x

    def to_dict(self) -> Dict[str, float]:
        if self.n == 0:
            return {}
        var = self._m2 / self.n if self.n > 0 else 0.0
        return {
            "min": self.min,
            "max": self.max,
            "avg": self.mean,
            "std": math.sqrt(var),
        }


class InstrDistributionHTMLGenerator(PlotlyReporterBase):
    INSTRS_TO_EXCLUDE = [
        "MOV_SPR_XN",
        "DFX_REGION",
        "DFX_REGION_V",
    ]

    ORDERED_TYPES = [
        InstrType.MTE1,
        InstrType.MTE2,
        InstrType.MTE3,
        InstrType.FIXP,
        InstrType.CUBE,
        InstrType.PUSHQ,
        InstrType.SCALAR,
        InstrType.FC,
        InstrType.RVECEX,
        InstrType.RVECLP,
        InstrType.RVECSU,
        InstrType.RVECST,
        InstrType.RVECLD,
    ]

    PIPES = [
        InstrType.CUBE,
        InstrType.FIXP,
        InstrType.MTE1,
        InstrType.MTE2,
        InstrType.MTE3,
        InstrType.PUSHQ,
    ]

    MAX_ITEMS_IN_HOVER = 10

    @staticmethod
    def is_not_sync_instr(instr_name: str) -> bool:
        return all(x not in instr_name for x in SYNCHRO_INSTRS)

    def _keep_instr(self, instr_name: str) -> bool:
        return (
            self.is_not_sync_instr(instr_name)
            and instr_name not in self.INSTRS_TO_EXCLUDE
        )

    @staticmethod
    def iter_instrs() -> Iterator[sqlite3.Row]:
        return database_handler.iter_rows("""
        SELECT ExecInstrId, InstrTypeId, ExecInstrName, SourceInstrAddr,
               ExecInstrTickEnd - ExecInstrTickStart AS Duration
        FROM ExecutedInstructions
        """)

    @staticmethod
    def _stats_row(stats: Dict[str, float]) -> str:
        if not stats:
            return ""
        return (
            f"<td>{stats['min']}</td><td>{stats['max']}</td>"
            f"<td>{stats['avg']:.2f}</td><td>{stats['std']:.2f}</td>"
        )

    def _prepare_bar_data(
        self, instr_data: List[Tuple[int, int, int]]
    ) -> Tuple[List[int], List[int], List[str]]:
        if not instr_data:
            return [], [], []
        duration_to_instrs: Dict[int, List[Tuple[int, int]]] = defaultdict(list)
        for dur, eid, addr in instr_data:
            duration_to_instrs[dur].append((eid, addr))
        sorted_durs = sorted(duration_to_instrs.keys())
        y = [len(duration_to_instrs[d]) for d in sorted_durs]
        customdata = []
        for d in sorted_durs:
            bucket = duration_to_instrs[d]
            total = len(bucket)
            if total > self.MAX_ITEMS_IN_HOVER:
                shown = sorted(bucket)[: self.MAX_ITEMS_IN_HOVER]
                extra = f"<br>and {total - self.MAX_ITEMS_IN_HOVER} more"
            else:
                shown = sorted(bucket)
                extra = ""
            instr_str = "<br>".join(
                f"ID: {eid}, PC: {hex(addr)}" for eid, addr in shown
            )
            customdata.append(instr_str + extra)
        return sorted_durs, y, customdata

    def _generate_type_distribution_figure(
        self,
        instrs_by_type_and_name: Dict[int, Dict[str, List[Tuple[int, int, int]]]],
    ) -> go.Figure:
        fig = go.Figure()

        order_dict = {typ.value: idx for idx, typ in enumerate(self.ORDERED_TYPES)}
        sorted_type_ids = sorted(
            instrs_by_type_and_name.keys(),
            key=lambda tid: order_dict.get(tid, len(order_dict)),
        )

        for instr_type_id in sorted_type_ids:
            instr_data: List[Tuple[int, int, int]] = []
            for lst in instrs_by_type_and_name[instr_type_id].values():
                instr_data.extend(lst)

            instr_type = InstrType(instr_type_id)
            is_visible = True if instr_type in self.PIPES else "legendonly"

            x, y, customdata = self._prepare_bar_data(instr_data)
            del instr_data
            if not x:
                continue

            fig.add_trace(
                go.Bar(
                    x=x,
                    y=y,
                    name=instr_type.name,
                    customdata=customdata,
                    hovertemplate=(
                        "Duration: %{x}<br>Count: %{y}<br>"
                        "Instructions:<br>%{customdata}<extra></extra>"
                    ),
                    opacity=0.75,
                    visible=is_visible,
                    width=1,
                    marker_line_width=0,
                )
            )

        fig.update_layout(
            title="Instruction Duration Distribution by Type",
            xaxis_title="Duration (ticks)",
            yaxis_title="Count",
            barmode="overlay",
        )
        return fig

    def _generate_per_type_figures(
        self,
        instrs_by_type_and_name: Dict[int, Dict[str, List[Tuple[int, int, int]]]],
    ) -> Dict[int, go.Figure]:
        fig_per_type: Dict[int, go.Figure] = {}
        for instr_type_id, name_instrs in instrs_by_type_and_name.items():
            if not name_instrs:
                continue
            fig = go.Figure()
            type_name = InstrType(instr_type_id).name

            for name in sorted(name_instrs.keys()):
                x, y, customdata = self._prepare_bar_data(name_instrs[name])
                if not x:
                    continue

                fig.add_trace(
                    go.Bar(
                        x=x,
                        y=y,
                        name=name,
                        customdata=customdata,
                        hovertemplate=(
                            "Duration: %{x}<br>Count: %{y}<br>"
                            "Instructions:<br>%{customdata}<extra></extra>"
                        ),
                        opacity=0.75,
                        width=1,
                        marker_line_width=0,
                    )
                )

            fig.update_layout(
                title=f"Instruction Duration Distribution for {type_name}",
                xaxis_title="Duration (ticks)",
                yaxis_title="Count",
                barmode="overlay",
                showlegend=True,
            )
            fig_per_type[instr_type_id] = fig
        return fig_per_type

    def _process(self) -> None:
        # type -> name -> list[(duration, exec_id, addr_int)]
        instrs_by_type_and_name: Dict[int, Dict[str, List[Tuple[int, int, int]]]] = (
            defaultdict(lambda: defaultdict(list))
        )

        # Online stats accumulators
        overall_acc = RunningStats()
        type_accs: Dict[int, RunningStats] = {}
        name_accs: Dict[int, Dict[str, RunningStats]] = defaultdict(dict)

        for instr in self.iter_instrs():
            instr_name = instr["ExecInstrName"]
            if not self._keep_instr(instr_name):
                continue

            instr_type_id = instr["InstrTypeId"]
            duration = instr["Duration"]
            exec_id = decompose_instr_id(instr["ExecInstrId"])[0]

            addr = instr["SourceInstrAddr"]

            instrs_by_type_and_name[instr_type_id][instr_name].append(
                (duration, exec_id, addr)
            )

            overall_acc.add(duration)
            tacc = type_accs.get(instr_type_id)
            if tacc is None:
                tacc = RunningStats()
                type_accs[instr_type_id] = tacc
            tacc.add(duration)
            nacc = name_accs[instr_type_id].get(instr_name)
            if nacc is None:
                nacc = RunningStats()
                name_accs[instr_type_id][instr_name] = nacc
            nacc.add(duration)

        overall_stats = overall_acc.to_dict()
        type_stats = {tid: acc.to_dict() for tid, acc in type_accs.items()}
        name_stats = {
            tid: {n: a.to_dict() for n, a in d.items()} for tid, d in name_accs.items()
        }

        fig_types = self._generate_type_distribution_figure(instrs_by_type_and_name)
        fig_per_type = self._generate_per_type_figures(instrs_by_type_and_name)

        # Bulk data is no longer needed once figures are built
        instrs_by_type_and_name.clear()

        html_types = pio.to_html(fig_types, full_html=False, include_plotlyjs=True)
        html_per_types = {
            instr_type_id: pio.to_html(fig, full_html=False, include_plotlyjs=False)
            for instr_type_id, fig in fig_per_type.items()
        }
        fig_per_type.clear()

        # Overall stats table with types and all
        overall_table = (
            "<table>\n<tr><th>Type</th><th>Min (ticks)</th>"
            "<th>Max (ticks)</th><th>Mean (ticks)</th>"
            "<th>Standard deviation</th></tr>\n"
        )
        present_types = [typ for typ in self.ORDERED_TYPES if typ.value in type_stats]
        for typ in present_types:
            stats = type_stats[typ.value]
            overall_table += f"<tr><td>{typ.name}</td>{self._stats_row(stats)}</tr>\n"
        overall_table += f"<tr><td>All</td>{self._stats_row(overall_stats)}</tr>\n"
        overall_table += "</table>"
        overall_stats_html = f"<h2>Overall Stats</h2>{overall_table}"

        # Generate per-type HTML sections in the specified order,
        # skipping absent types
        per_type_sections = ""
        for typ in self.ORDERED_TYPES:
            type_id = typ.value
            if type_id not in html_per_types:
                continue
            html_graph = html_per_types[type_id]
            per_type_table = (
                "<table>\n<tr><th>Instruction</th><th>Min (ticks)</th>"
                "<th>Max (ticks)</th><th>Mean (ticks)</th>"
                "<th>Standard deviation</th></tr>\n"
            )
            stats = type_stats.get(type_id, {})
            per_type_table += (
                f"<tr><td>Type Overall</td>{self._stats_row(stats)}</tr>\n"
            )
            for name in sorted(name_stats.get(type_id, {}).keys()):
                stats = name_stats[type_id][name]
                per_type_table += f"<tr><td>{name}</td>{self._stats_row(stats)}</tr>\n"
            per_type_table += "</table>"
            per_type_sections += (
                f"<h3>{typ.name}</h3>{html_graph}<h4>Stats</h4>"
                f'{per_type_table}<div style="height:20px;"></div>'
            )

        combined_html = f"""
        <html>
        <head>
            <style>
                body {{
                    font-family: Arial, sans-serif;
                }}
                table {{
                    border-collapse: collapse;
                    width: 100%;
                    font-family: Arial, sans-serif;
                    margin-bottom: 20px;
                }}
                th, td {{
                    border: 1px solid #dddddd;
                    text-align: left;
                    padding: 12px;
                }}
                th {{
                    background-color: #f2f2f2;
                    font-weight: bold;
                }}
                tr:nth-child(even) {{
                    background-color: #f9f9f9;
                }}
                tr:hover {{
                    background-color: #f5f5f5;
                }}
            </style>
        </head>
        <body>
            <h1>Instruction Duration Distributions</h1>
            {html_types}
            {overall_stats_html}
            <h2>Per-Type Details</h2>
            {per_type_sections}
        </body>
        </html>
        """
        with open(
            f"{self.output_dir}/qi_instruction_duration_distribution_report.html", "w+"
        ) as f:
            f.write(combined_html)

    def process(self) -> None:
        with StageReporter("[QI] Instruction duration distribution report generation"):
            self._process()
