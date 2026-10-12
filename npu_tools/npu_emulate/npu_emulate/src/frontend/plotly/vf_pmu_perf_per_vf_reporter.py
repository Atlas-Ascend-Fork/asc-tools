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

"""Per-VF Perf Summary report: one HTML page per (core, core type), with one
card/row per VF.

The whole-subcore twin of this report lives in
vf_pmu_perf_summary_reporter.py; shared formatting helpers and constants
live in vf_pmu_perf_common.py.
"""

import os
from typing import Dict, List

from npu_emulate.src.backend.analyzers.vf_pmu import storage
from npu_emulate.src.backend.analyzers.vf_pmu.metrics import display_name
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.common.biprof_enum import CoreType
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.frontend.base.plotly_reporter_base import (
    PlotlyReporterBase,
)
from npu_emulate.src.frontend.plotly.vf_pmu_perf_common import (
    _L1_NAMES,
    _TD_L1_COLORS,
    IDU_WIDTH,
    ISU_MAX_EX,
    ISU_MAX_GA,
    ISU_MAX_LD,
    ISU_MAX_ST,
    _assert_topdown_names,
    _fmt,
    _heat,
    _pct,
)

_PIPE_NAMES = [
    ("RVECEX", "c_slot_idu_disp_rvecex", "EX (Compute)", 2),
    ("RVECLD", "c_slot_idu_disp_rvecld_ld", "LD (Load)", 2),
    ("RVECST", "c_slot_idu_disp_rvecst_st", "ST (Store)", 1),
    ("RVECSU", "c_slot_idu_disp_rvecsu", "SU (Scalar)", 1),
    ("RVECLP", "c_slot_idu_disp_rveclp", "LP (Loop)", 1),
]

_INSTR_CSS = """<style>
*{box-sizing:border-box;margin:0;padding:0}
body{
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
  background:#f5f5f5;color:#333;padding:20px}
.container{max-width:1200px;margin:0 auto}
h1{
  color:#1a1a2e;border-bottom:3px solid #4a90d9;padding-bottom:8px;
  margin-bottom:20px}
.metric{display:inline-block;margin-right:24px;margin-bottom:8px}
.metric .label{color:#888;font-size:0.85em}
.metric .value{font-size:1.3em;font-weight:bold;color:#2c3e50}
details{margin:12px 0}
details>summary{
  cursor:pointer;list-style:none;display:flex;align-items:center;gap:8px;
  user-select:none}
details>summary::-webkit-details-marker{display:none}
details>summary::before{
  content:'\\25b6';font-size:0.7em;color:#888;transition:transform .2s}
details[open]>summary::before{transform:rotate(90deg)}
details>summary:hover{color:#4a90d9}
details>.collapsible-body{padding:4px 0 4px 12px}
.fragment{
  background:#fff;border:1px solid #ddd;border-radius:8px;padding:16px 20px;
  margin:12px 0}
.data-table{
  width:100%;border-collapse:collapse;font-size:0.9em;margin:10px 0}
.data-table th{
  background:#4a90d9;color:#fff;padding:6px 10px;text-align:left;
  border:1px solid #ddd}
.data-table td{border:1px solid #ddd;padding:6px 10px}
.data-table tr:nth-child(even){background:#f8f8f8}
.data-table tr:hover{background:#e8f0fe}
.label-cell{font-weight:600;white-space:nowrap}
.bar-cell{padding:2px 4px!important;width:40%}
.td-bar{
  height:28px;background:#eee;border-radius:4px;display:flex;
  overflow:hidden;margin:4px 0}
.td-seg{transition:width .2s}
.legend-item{display:inline-block;margin-right:24px}
.legend-color{
  display:inline-block;width:10px;height:10px;border-radius:2px;
  vertical-align:middle;margin-right:4px}
.kpi-row{display:flex;flex-wrap:wrap;gap:8px;margin:10px 0}
.finding{
  background:#fff3e0;border-left:3px solid #ff9800;padding:8px 12px;
  margin:8px 0;font-size:0.9em;border-radius:0 4px 4px 0}
.insight{
  background:#e8f5e9;border-left:3px solid #4caf50;padding:8px 12px;
  margin:8px 0;font-size:0.9em;border-radius:0 4px 4px 0}
footer{
  margin-top:30px;text-align:center;color:#aaa;font-size:0.8em;
  border-top:1px solid #eee;padding-top:10px}
</style>"""


def _fetch_vf_topdown(core_id: int, core_type_id: int) -> Dict[int, Dict[str, float]]:
    """{vf_id: {display_name: value}} for the per-VF hierarchy.

    Read, not recomputed: the analyzer already evaluated these from the same
    per-VF deltas, so the numbers agree with the per-subcore report by
    construction. A VF whose syn_c_optimal was <= 0 is simply absent.
    """
    by_vf = storage.fetch_vf_synth_values(core_id, core_type_id, min_level=1)
    return {
        vf_id: {display_name(name): value for name, value in values.items()}
        for vf_id, values in by_vf.items()
    }


def _compute_ipc(raw: Dict[str, float], busy: float) -> Dict[str, dict]:
    """Per-VF IPC, computed entirely from RAW deltas over kernel ticks (the
    `busy` arg). Distinct from the synthetic syn_c_ipc_*, which normalize by
    c_cycle_rvec_busy - so this stays a raw computation, not a synth read."""
    ex = raw.get("c_slot_idu_disp_rvecex", 0)
    ld = raw.get("c_slot_idu_disp_rvecld_ld", 0)
    st = raw.get("c_slot_idu_disp_rvecst_st", 0)
    ga = raw.get("c_slot_idu_disp_rvecld_ga", 0)
    sc = raw.get("c_slot_idu_disp_rvecst_sc", 0)
    total = raw.get("c_slot_idu_disp_total", 0)
    b = max(busy, 1)
    return {
        "EX": {
            "sim": ex / b,
            "peak": ISU_MAX_EX,
            "slots": ex,
            "ratio": ex / total if total else 0,
        },
        "LD": {
            "sim": ld / b,
            "peak": ISU_MAX_LD,
            "slots": ld,
            "ratio": ld / total if total else 0,
        },
        "GA": {
            "sim": ga / b,
            "peak": ISU_MAX_GA,
            "slots": ga,
            "ratio": ga / total if total else 0,
        },
        "ST": {
            "sim": st / b,
            "peak": ISU_MAX_ST,
            "slots": st,
            "ratio": st / total if total else 0,
        },
        "SC": {
            "sim": sc / b,
            "peak": ISU_MAX_ST,
            "slots": sc,
            "ratio": sc / total if total else 0,
        },
        "Total": {"sim": total / b, "peak": IDU_WIDTH, "slots": total, "ratio": 1.0},
    }


def _pipe_table(delta: Dict[str, float], ticks: float) -> str:
    """Pipe counts/ratios/IPC from RAW per-VF deltas (c_* keys) over ticks."""
    rows = []
    total = delta.get("c_slot_idu_disp_total", 0)
    for _, counter_key, label, _ in _PIPE_NAMES:
        cnt = delta.get(counter_key, 0)
        ratio = cnt / total if total > 0 else 0
        ipc = cnt / ticks if ticks > 0 else 0
        w = min(ratio * 100, 100)
        rows.append(
            f"<tr><td>{label}</td><td>{int(cnt):,}</td>"
            f"<td>{_pct(ratio)}</td>"
            f'<td class="bar-cell"><div style="background:#5dade2;'
            f"width:{w:.1f}%;height:18px;border-radius:3px;"
            f'min-width:1px;"></div></td>'
            f"<td>{_fmt(ipc)}</td></tr>"
        )
    header = (
        "<tr><th>Pipe</th><th>Count</th><th>Ratio</th>"
        "<th>Distribution</th><th>IPC</th></tr>"
    )
    return (
        f'<table class="data-table"><thead>{header}</thead>'
        f"<tbody>{''.join(rows)}</tbody></table>"
    )


def _td_bar_html(td: Dict[str, float]) -> str:
    """L1 stacked bar from the SYNTHETIC top-down hierarchy (display-named)."""
    l1_colors = [_TD_L1_COLORS[n] for n in _L1_NAMES]
    segs = []
    for name, color in zip(_L1_NAMES, l1_colors):
        v = td.get(name, 0)
        w = min(v * 100, 100)
        label = name.replace("_", " ").title()
        segs.append(
            f'<div class="td-seg" style="width:{w:.1f}%;height:100%;'
            f'background:{color};" title="{label}: {_pct(v)}"></div>'
        )
    legend = "".join(
        f'<span class="legend-item"><span class="legend-color" '
        f'style="background:{c}"></span>'
        f"{n.replace('_', ' ').title()}</span>"
        for n, c in zip(_L1_NAMES, l1_colors)
    )
    return (
        f'<div class="td-bar">{"".join(segs)}</div>'
        f'<div style="font-size:0.8em;color:#666;margin-top:2px;">'
        f"{legend}</div>"
    )


def _ipc_mini_bars(ipc: Dict) -> str:
    """Per-VF IPC bars; `ipc` is the raw-derived dict from _compute_ipc()."""
    parts = []
    for uname in ["EX", "LD", "GA", "ST"]:
        d = ipc[uname] if uname in ipc else ipc["Total"]
        v = d["sim"]
        peak = d["peak"]
        eff = v / peak if peak > 0 else 0
        w = min(eff * 100, 100)
        color = _heat(eff, 0.3, 0.6)
        parts.append(
            f'<div style="display:flex;align-items:center;gap:4px;'
            f'margin:2px 0;">'
            f'<span style="width:24px;font-size:0.8em;color:#666;">'
            f"{uname}</span>"
            f'<div style="flex:1;height:14px;background:#eee;'
            f'border-radius:3px;overflow:hidden;">'
            f'<div style="width:{w:.1f}%;height:100%;background:{color};'
            f'border-radius:3px;"></div>'
            f'</div><span style="font-size:0.8em;font-weight:600;'
            f'color:{color};min-width:40px;">{_fmt(v)}</span></div>'
        )
    return f'<div style="margin-top:6px;">{"".join(parts)}</div>'


class VfPMUPerfPerVfReporter(PlotlyReporterBase):
    def _process(self) -> None:
        storage.make_indexes()
        try:
            cores = BackendDBManager.get_active_cores()
            core_types = BackendDBManager.get_active_core_types_ordered()

            for core_id in cores:
                for core_type_id in core_types:
                    ct_name = CoreType(core_type_id).name
                    ticks_map = storage.fetch_vf_kernel_ticks(core_id, core_type_id)
                    if not ticks_map:
                        continue
                    vf_deltas = storage.fetch_vf_deltas(  # raw per-VF
                        core_id, core_type_id
                    )
                    td_data = _fetch_vf_topdown(  # synth top-down
                        core_id, core_type_id
                    )

                    page = self._build_page(
                        vf_deltas, ticks_map, td_data, core_id, ct_name
                    )
                    fname = f"pmu_perf_per_vf_core_{core_id}_{ct_name}.html"
                    path = os.path.join(self.output_dir, fname)
                    with open(path, "w") as f:
                        f.write(page)
        finally:
            storage.drop_indexes()

    def _build_page(
        self,
        vf_deltas: Dict[int, Dict[str, float]],
        ticks_map: Dict[int, int],
        td_data: Dict[int, Dict[str, float]],
        core_id: int,
        ct_name: str,
    ) -> str:
        vf_ids = sorted(vf_deltas.keys())
        if not vf_ids:
            return "<html><body>No VF data</body></html>"

        ticks = {vf: max(ticks_map.get(vf, 1), 1) for vf in vf_ids}

        ipc_data: Dict[int, Dict] = {
            vf: _compute_ipc(vf_deltas[vf], ticks[vf]) for vf in vf_ids
        }
        # A VF whose hierarchy is undefined has no row; treat it as empty.
        td_by_vf: Dict[int, Dict[str, float]] = {
            vf: td_data.get(vf, {}) for vf in vf_ids
        }

        sec1 = self._sec_per_vf(vf_ids, ipc_data, td_by_vf, ticks, vf_deltas)
        sec2 = self._sec_cross_vf(vf_ids, ipc_data, td_by_vf, ticks, vf_deltas)
        sec3 = self._sec_bottleneck(vf_ids, ipc_data, td_by_vf)

        return (
            f'<!DOCTYPE html><html lang="en"><head>'
            f'<meta charset="UTF-8">'
            f'<meta name="viewport" '
            f'content="width=device-width,initial-scale=1.0">'
            f"<title>V100 Per-VF Performance Report</title>"
            f"{_INSTR_CSS}</head><body>"
            f'<div class="container">'
            f"<h1>V100 VF Performance Report &mdash; "
            f"core_{core_id} {ct_name}</h1>"
            f"{sec1}{sec2}{sec3}"
            f"<footer>Generated by biprof &mdash; "
            f"{len(vf_ids)} VF fragments analyzed</footer>"
            f"</div></body></html>"
        )

    def _sec_per_vf(
        self,
        vf_ids: List[int],
        ipc_data: Dict,
        td_data: Dict,
        ticks: Dict,
        vf_deltas: Dict,
    ) -> str:
        blocks = []
        for idx, vf in enumerate(vf_ids):
            ipc = ipc_data[vf]  # raw-derived (over ticks)
            td = td_data[vf]  # synth top-down
            delta = vf_deltas[vf]  # raw deltas
            sim = ipc["Total"]["sim"]
            eff = sim / IDU_WIDTH
            total_instr = int(delta.get("c_slot_idu_disp_total", 0))
            color = _heat(eff, 0.3, 0.6)

            metrics = (
                f'<div class="metric"><div class="label">Instructions</div>'
                f'<div class="value">{total_instr:,}</div></div>'
                f'<div class="metric"><div class="label">Cycles</div>'
                f'<div class="value">{int(ticks[vf]):,}</div></div>'
                f'<div class="metric"><div class="label">IPC (sim)</div>'
                f'<div class="value" style="color:{color}">'
                f"{_fmt(sim)}</div></div>"
                f'<div class="metric"><div class="label">Efficiency</div>'
                f'<div class="value">{_pct(eff)}</div></div>'
            )

            pipe_tbl = _pipe_table(delta, ticks[vf])
            td_bar = _td_bar_html(td)
            ipc_bars = _ipc_mini_bars(ipc)

            block = (
                f"<details{' open' if idx < 3 else ''}>"
                f"<summary><h3>VF {vf} &mdash; "
                f"IPC: {_fmt(sim)}, Efficiency: {_pct(eff)}</h3></summary>"
                f'<div class="collapsible-body">'
                f'<div class="fragment">'
                f'<div class="kpi-row">{metrics}</div>'
                f'<h4 style="margin-top:12px;">Pipe Distribution</h4>'
                f"{pipe_tbl}"
                f'<h4 style="margin-top:12px;">Top-Down L1</h4>'
                f"{td_bar}"
                f'<h4 style="margin-top:12px;">IPC by Unit</h4>'
                f"{ipc_bars}"
                f"</div></div></details>"
            )
            blocks.append(block)

        body = "".join(blocks)
        return (
            f"<details open><summary><h2>1. Per-VF Analysis</h2>"
            f"</summary>"
            f'<div class="collapsible-body">{body}</div></details>'
        )

    def _sec_cross_vf(
        self,
        vf_ids: List[int],
        ipc_data: Dict,
        td_data: Dict,
        ticks: Dict,
        vf_deltas: Dict,
    ) -> str:
        header = (
            "<tr><th>VF</th><th>Instr</th><th>Cycles</th>"
            "<th>IPC</th><th>Efficiency</th>"
            "<th>Retired</th><th>Frontend</th>"
            "<th>Backend ASU</th><th>Backend Vec</th></tr>"
        )
        rows = []
        for vf in vf_ids:
            ipc = ipc_data[vf]  # raw-derived (over ticks)
            td = td_data[vf]  # synth top-down
            delta = vf_deltas[vf]  # raw deltas
            sim = ipc["Total"]["sim"]
            eff = sim / IDU_WIDTH
            total_instr = int(delta.get("c_slot_idu_disp_total", 0))
            ipc_color = _heat(eff, 0.3, 0.6)
            ret_v = td.get("retired", 0)
            fb_v = td.get("frontend_bound", 0)
            asu_v = td.get("backend_asu_bound", 0)
            vec_v = td.get("backend_vec_bound", 0)

            rows.append(
                f"<tr>"
                f'<td class="label-cell">VF {vf}</td>'
                f"<td>{total_instr:,}</td>"
                f"<td>{int(ticks[vf]):,}</td>"
                f'<td style="font-weight:600;color:{ipc_color}">'
                f"{_fmt(sim)}</td>"
                f"<td>{_pct(eff)}</td>"
                f'<td style="color:{_heat(ret_v)}">{_pct(ret_v)}</td>'
                f'<td style="color:{_heat(fb_v)}">{_pct(fb_v)}</td>'
                f'<td style="color:{_heat(asu_v)}">{_pct(asu_v)}</td>'
                f'<td style="color:{_heat(vec_v)}">{_pct(vec_v)}</td>'
                f"</tr>"
            )

        body = (
            f'<div style="overflow-x:auto">'
            f'<table class="data-table"><thead>{header}</thead>'
            f"<tbody>{''.join(rows)}</tbody></table></div>"
        )
        return (
            f"<details open><summary><h2>2. Cross-VF Comparison</h2>"
            f"</summary>"
            f'<div class="collapsible-body">{body}</div></details>'
        )

    def _sec_bottleneck(self, vf_ids: List[int], ipc_data: Dict, td_data: Dict) -> str:
        if not vf_ids:
            return ""

        total_ipcs = [(vf, ipc_data[vf]["Total"]["sim"]) for vf in vf_ids]
        avg_ipc = sum(v for _, v in total_ipcs) / len(total_ipcs)
        best = max(total_ipcs, key=lambda x: x[1])
        worst = min(total_ipcs, key=lambda x: x[1])

        # Averaged over VFs, restricted to L1: td_data carries the whole
        # hierarchy, and taking max() over all of it would compare a level 1
        # root against a level 4 leaf.
        agg_l1: Dict[str, float] = {}
        for name in _L1_NAMES:
            agg_l1[name] = sum(td_data[vf].get(name, 0) for vf in vf_ids) / len(vf_ids)

        dominant = max(agg_l1, key=lambda name: agg_l1[name])
        dom_label = dominant.replace("_", " ").title()
        dom_val = agg_l1[dominant]

        body = (
            f'<div class="finding"><strong>IPC Range:</strong> '
            f"Average = {_fmt(avg_ipc)}, "
            f"Best VF = {best[0]} ({_fmt(best[1])}), "
            f"Worst VF = {worst[0]} ({_fmt(worst[1])}).</div>"
            f'<div class="insight"><strong>Dominant Bottleneck:</strong> '
            f"{dom_label} at {_pct(dom_val)} average across all VFs.</div>"
            f"<h3>IPC Distribution</h3>"
            f'<table class="data-table"><thead>'
            f"<tr><th>VF</th><th>IPC</th><th>Deviation</th></tr>"
            f"</thead><tbody>"
        )
        for vf, ipc_val in total_ipcs:
            dev = ipc_val - avg_ipc
            dev_pct = (dev / avg_ipc * 100) if avg_ipc > 0 else 0
            if dev >= 0:
                dev_str = f"+{_fmt(dev)}"
                bar_color = "#4caf50"
            else:
                dev_str = _fmt(dev)
                bar_color = "#f44336"
            worst_mark = " &#10052;" if vf == worst[0] and len(vf_ids) > 1 else ""
            body += (
                f'<tr><td class="label-cell">VF {vf}{worst_mark}</td>'
                f'<td style="font-weight:600;">{_fmt(ipc_val)}</td>'
                f'<td style="color:{bar_color};">{dev_str} '
                f"({_pct(abs(dev_pct) / 100)})</td></tr>"
            )
        body += "</tbody></table>"

        return (
            f"<details open><summary>"
            f"<h2>3. Performance Bottleneck Analysis</h2></summary>"
            f'<div class="collapsible-body">{body}</div></details>'
        )

    def process(self) -> None:
        with StageReporter("[PMU] Per-VF performance report generation"):
            self._process()
        final_logger.add_general_info(f"[PMU] Per-VF reports: {self.output_dir}")


_assert_topdown_names(set(_L1_NAMES), "vf_pmu_perf_per_vf_reporter")
