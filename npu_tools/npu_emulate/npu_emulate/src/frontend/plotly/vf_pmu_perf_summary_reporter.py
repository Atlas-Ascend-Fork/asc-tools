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

"""Whole-subcore Perf Summary report: one HTML page per (core, core type).

The per-VF twin of this report (one card/row per VF instead of one number
per subcore) lives in vf_pmu_perf_per_vf_reporter.py; shared formatting
helpers and constants live in vf_pmu_perf_common.py.
"""

import html as html_mod
import os
import zlib
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
    _bar,
    _fmt,
    _heat,
    _pct,
    _section,
)

_CSS = """<style>
*{box-sizing:border-box;margin:0;padding:0}
body{
  font-family:-apple-system,'Segoe UI',Roboto,sans-serif;
  background:#f5f5f5;color:#333;padding:20px}
.container{max-width:1200px;margin:0 auto}
h1{
  text-align:center;padding:20px 0;color:#1a237e;
  border-bottom:2px solid #1a237e;margin-bottom:20px}
.dashboard{display:flex;gap:12px}
.kpi-card{
  flex:1;min-width:0;
  background:#fafafa;border:1px solid #e0e0e0;border-radius:6px;
  padding:12px;text-align:center}
.kpi-label{font-size:.85em;color:#666;margin-bottom:4px}
.kpi-value{font-size:1.6em;font-weight:700;color:#1a237e}
.kpi-card.accent{border-color:#ff9800;background:#fff3e0}
.kpi-card.accent .kpi-value{color:#e65100}
.kpi-card.warn{border-color:#f44336;background:#ffebee}
.kpi-card.warn .kpi-value{color:#c62828;font-size:1.2em}
.data-table{width:100%;border-collapse:collapse;font-size:.9em}
.data-table th{
  background:#e8eaf6;color:#1a237e;padding:8px 10px;text-align:left;
  border-bottom:2px solid #c5cae9}
.data-table td{padding:6px 10px;border-bottom:1px solid #eee}
.data-table tr:hover{background:#f5f5f5}
.label-cell{font-weight:600;white-space:nowrap}
.eff-cell{white-space:nowrap}
.legend{margin-top:10px;font-size:.85em;color:#666}
.legend-item{display:inline-block;margin-right:24px}
.legend-color{
  display:inline-block;width:12px;height:12px;border-radius:2px;
  vertical-align:middle;margin-right:4px}
details{
  margin:12px 0;background:#fff;border-radius:8px;
  box-shadow:0 1px 3px rgba(0,0,0,.1);overflow:hidden}
details>summary{
  cursor:pointer;list-style:none;display:flex;align-items:center;gap:8px;
  user-select:none;padding:14px 20px;border-bottom:1px solid #e0e0e0}
details>summary::-webkit-details-marker{display:none}
details>summary::before{
  content:'\\25b6';font-size:.7em;color:#666;transition:transform .2s}
details[open]>summary::before{transform:rotate(90deg)}
details>summary:hover{background:#f5f5ff}
details>summary h2{color:#1a237e;margin:0;font-size:1.3em}
.collapsible-body{padding:20px}
.rpt-note{
  background:#f5f5f5;border-top:1px solid #e0e0e0;padding:10px 16px;
  font-size:.8em;color:#888;font-family:monospace;line-height:1.5;
  white-space:pre-wrap}
.rpt-note-title{
  font-family:sans-serif;font-weight:600;color:#666;margin-bottom:4px;
  font-size:.95em}
.unit-cards-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}
.unit-card{
  background:#fafafa;border:1px solid #e0e0e0;border-radius:6px;padding:12px}
.unit-card h4{color:#1a237e;margin-bottom:6px;font-size:1em}
.mini-table{width:100%;font-size:.85em}
.mini-table td{padding:2px 4px}
.tree-label{display:inline-block}
.td-toggle{
  cursor:pointer;user-select:none;display:inline-flex;align-items:center;
  gap:4px}
.td-arrow{
  display:inline-block;width:14px;height:14px;text-align:center;
  line-height:14px;font-size:.7em;color:#666;transition:transform .15s;
  border-radius:2px;background:#e8eaf6}
.td-arrow.collapsed{transform:rotate(-90deg)}
.td-leaf-spacer{display:inline-block;width:14px}
.td-child-rows{transition:opacity .15s}
.td-child-rows.hidden{display:none}
.analysis-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.analysis-card{
  background:#fafafa;border:1px solid #e0e0e0;border-radius:6px;padding:16px}
.analysis-card.full-width{grid-column:1/-1}
.analysis-card h3{color:#1a237e;font-size:1.05em;margin-bottom:8px}
.finding{
  background:#fff3e0;border-left:3px solid #ff9800;padding:8px 12px;
  margin:8px 0;font-size:.9em}
.insight{
  background:#e8f5e9;border-left:3px solid #4caf50;padding:8px 12px;
  margin:8px 0;font-size:.9em}
.warning{
  background:#ffebee;border-left:3px solid #f44336;padding:8px 12px;
  margin:8px 0;font-size:.9em}
.bar{
  display:inline-block;height:18px;background:#eee;border-radius:3px;
  vertical-align:middle;position:relative}
.bar-fill{height:100%;border-radius:3px}
.bar-label{
  position:absolute;right:8px;top:1px;font-size:11px;font-weight:600;
  font-family:monospace;color:#475569}
</style>"""

# Row identity is a dotted path ("1.3.2"), so a node's descendants are exactly
# the rows whose path starts with "<path>.". Keying off row ids and testing
# startsWith("td-3-") never matched, so collapsing a node used to hide its
# children but leave the grandchildren on screen.
_TD_TREE_JS = """<script>
function tdToggle(el){
  var cur=el.closest('tr'), path=cur.dataset.path, tbl=cur.parentNode;
  var hide=cur.dataset.expanded!=='0';
  cur.dataset.expanded=hide?'0':'1';
  var arrow=cur.querySelector('.td-arrow');
  if(arrow){arrow.classList.toggle('collapsed',hide);}
  tbl.querySelectorAll('tr[data-path^="'+path+'."]').forEach(function(r){
    if(hide){r.style.display='none';return;}
    // On expand, a descendant reappears only if every ancestor between the
    // toggled node and it is itself expanded.
    var rel=r.dataset.path.slice(path.length+1).split('.');
    var p=path, visible=true;
    for(var i=0;i<rel.length-1;i++){
      p=p+'.'+rel[i];
      var anc=tbl.querySelector('tr[data-path="'+p+'"]');
      if(anc&&anc.dataset.expanded==='0'){visible=false;break;}
    }
    r.style.display=visible?'':'none';
  });
}
</script>"""


# ---------------------------------------------------------------------------
# Fetch helpers: storage speaks storage names, the report speaks display names
# ---------------------------------------------------------------------------


def _fetch_synth_subcore(core_id: int, core_type_id: int) -> Dict[str, float]:
    """{display_name: value} for every synthetic counter of one subcore.

    The report reads IPC, ratios, UB bandwidth and utilization from here
    instead of recomputing them from raw.
    """
    values = storage.fetch_subcore_synth_values(core_id, core_type_id)
    return {display_name(name): value for name, value in values.items()}


def _fetch_topdown_nodes(core_id: int, core_type_id: int) -> List[dict]:
    """Hierarchy nodes of one subcore, display-named, parents first.

    The rows already carry name and value, so this single call feeds both the
    L1 waterfall and the collapsible tree; a separate values query would hit
    the same view with the same WHERE.
    """
    nodes = storage.fetch_subcore_topdown_nodes(core_id, core_type_id)
    for node in nodes:
        node["name"] = display_name(node["name"])
        if node["parent_name"] is not None:
            node["parent_name"] = display_name(node["parent_name"])
    return nodes


# ---------------------------------------------------------------------------
# Plain-counter arithmetic
# ---------------------------------------------------------------------------


def _ipc_from_synth(synth: Dict[str, float], raw: Dict[str, float]) -> Dict[str, dict]:
    """Per-subcore IPC table. sim/ratio are synthetics (normalized by
    c_cycle_rvec_busy); only slot counts are raw and peaks are constants.
    Not the same as per-VF _compute_ipc(), which divides by kernel ticks.
    """
    ex = raw.get("c_slot_idu_disp_rvecex", 0)
    ld = raw.get("c_slot_idu_disp_rvecld_ld", 0)
    st = raw.get("c_slot_idu_disp_rvecst_st", 0)
    ga = raw.get("c_slot_idu_disp_rvecld_ga", 0)
    sc = raw.get("c_slot_idu_disp_rvecst_sc", 0)
    su = raw.get("c_slot_idu_disp_rvecsu", 0)
    lp = raw.get("c_slot_idu_disp_rveclp", 0)
    total = raw.get("c_slot_idu_disp_total", 0)
    busy = raw.get("c_cycle_rvec_busy", 1)
    return {
        "EX": {
            "sim": synth.get("ipc_ex", 0),
            "peak": ISU_MAX_EX,
            "slots": ex,
            "ratio": synth.get("ratio_ex", 0),
        },
        "LD": {
            "sim": synth.get("ipc_ld", 0),
            "peak": ISU_MAX_LD,
            "slots": ld,
            "ratio": synth.get("ratio_ld", 0),
        },
        "ST": {
            "sim": synth.get("ipc_st", 0),
            "peak": ISU_MAX_ST,
            "slots": st,
            "ratio": synth.get("ratio_st", 0),
        },
        "GA": {
            "sim": synth.get("ipc_ga", 0),
            "peak": ISU_MAX_GA,
            "slots": ga,
            "ratio": synth.get("ratio_ga", 0),
        },
        "SC": {
            "sim": synth.get("ipc_sc", 0),
            "peak": ISU_MAX_ST,
            "slots": sc,
            "ratio": synth.get("ratio_sc", 0),
        },
        "SU": {
            "sim": su / max(busy, 1),
            "peak": 1,
            "slots": su,
            "ratio": synth.get("ratio_su", 0),
        },
        "LP": {
            "sim": lp / max(busy, 1),
            "peak": 1,
            "slots": lp,
            "ratio": synth.get("ratio_lp", 0),
        },
        "Total": {
            "sim": synth.get("ipc_total", 0),
            "peak": IDU_WIDTH,
            "slots": total,
            "ratio": 1.0,
        },
    }


def _ub_bw_from_synth(synth: Dict[str, float]) -> Dict[str, dict]:
    """UB bandwidth table: sim from syn_c_ub_bw_*; est peaks are display
    constants, kept distinct from metrics.UB_PEAK_LSU."""
    return {
        "ldu": {"sim": synth.get("ub_bw_ldu", 0), "est": 512.0},
        "stu": {"sim": synth.get("ub_bw_stu", 0), "est": 256.0},
        "lsu": {"sim": synth.get("ub_bw_lsu", 0), "est": 768.0},
        "gsu": {"sim": synth.get("ub_bw_gsu", 0), "est": 128.0},
    }


def _util_from_synth(synth: Dict[str, float]) -> Dict[str, float]:
    return {
        name: synth.get(f"util_{name}", 0.0)
        for name in ["vreg", "preg", "ldq", "shq", "exq0", "exq1"]
    }


_TD_DEEP_COLORS = [
    "#ef5350",
    "#ff7043",
    "#ff8a65",
    "#ff5722",
    "#d84315",
    "#e53935",
    "#64b5f6",
    "#9e9e9e",
]


def _td_color(name: str) -> str:
    if name in _TD_L1_COLORS:
        return _TD_L1_COLORS[name]
    # crc32, not hash(): str hashing is salted per process (PYTHONHASHSEED),
    # so hash() gave a node a different color on every run.
    idx = zlib.crc32(name.encode()) % len(_TD_DEEP_COLORS)
    return _TD_DEEP_COLORS[idx]


def _build_td_tree_html(nodes: List[dict]) -> str:
    """Collapsible tree. Nodes are keyed by name; ids stay in storage."""
    by_name = {n["name"]: n for n in nodes}
    children_map: Dict[str, List[str]] = {}
    roots: List[str] = []
    for n in nodes:
        parent = n["parent_name"]
        if parent is None or parent not in by_name:
            roots.append(n["name"])
        else:
            children_map.setdefault(parent, []).append(n["name"])

    rows: List[str] = []
    counter = [0]

    def _render(name: str, depth: int, path: str) -> None:
        node = by_name[name]
        counter[0] += 1
        my_path = f"{path}.{counter[0]}" if path else str(counter[0])
        val = node["value"]
        pct_str = _pct(val)
        color = _td_color(name)
        kids = children_map.get(name, [])
        indent = depth * 30
        label = html_mod.escape(name.replace("_", " ").title())
        bar_w = min(val * 100, 100)

        cls = "" if not path else ' class="td-child-rows"'
        style = f"border-left:3px solid {color};padding-left:6px;"

        if kids:
            handle = (
                f'<span class="td-toggle" onclick="tdToggle(this)">'
                f'<span class="td-arrow">&#9660;</span>'
                f'<span class="tree-label" style="{style}">'
                f"{label}</span></span>"
            )
        else:
            handle = (
                f'<span class="td-leaf-spacer"></span>'
                f'<span class="tree-label" style="{style}">'
                f"{label}</span>"
            )

        rows.append(
            f'<tr{cls} data-path="{my_path}" data-expanded="1">'
            f'<td style="padding-left:{indent}px;">{handle}</td>'
            f"<td>{_bar(bar_w, color, 150)}</td>"
            f'<td style="text-align:right;font-weight:600;color:{color};">'
            f"{pct_str}</td></tr>"
        )

        for kid in kids:
            _render(kid, depth + 1, my_path)

    for root in roots:
        _render(root, 0, "")

    return "".join(rows)


class VfPMUPerfSummaryReporter(PlotlyReporterBase):
    def _process(self) -> None:
        storage.make_indexes()
        try:
            cores = BackendDBManager.get_active_cores()
            core_types = BackendDBManager.get_active_core_types_ordered()

            for core_id in cores:
                for core_type_id in core_types:
                    ct_name = CoreType(core_type_id).name
                    raw = storage.fetch_subcore_metrics(core_id, core_type_id)
                    if not raw:
                        continue

                    synth = _fetch_synth_subcore(core_id, core_type_id)
                    nodes = _fetch_topdown_nodes(core_id, core_type_id)
                    td = {
                        n["name"]: n["value"]  # synth top-down
                        for n in nodes
                    }

                    busy = raw.get("c_cycle_rvec_busy", 1)  # raw cycle count
                    ipc = _ipc_from_synth(synth, raw)
                    ub_bw = _ub_bw_from_synth(synth)
                    util = _util_from_synth(synth)
                    total_vfs = len(
                        storage.fetch_vf_kernel_ticks(core_id, core_type_id)
                    )

                    page = self._build_page(
                        raw,
                        synth,
                        td,
                        nodes,
                        ipc,
                        ub_bw,
                        util,
                        core_id,
                        ct_name,
                        busy,
                        total_vfs,
                    )
                    fname = f"pmu_perf_summary_core_{core_id}_{ct_name}.html"
                    path = os.path.join(self.output_dir, fname)
                    with open(path, "w") as f:
                        f.write(page)
        finally:
            storage.drop_indexes()

    def _build_page(
        self,
        raw: Dict,
        synth: Dict,
        td: Dict,
        nodes: List[dict],
        ipc: Dict,
        ub_bw: Dict,
        util: Dict,
        core_id: int,
        ct_name: str,
        busy: float,
        total_vfs: int,
    ) -> str:
        back_busy = raw.get("c_cycle_rvec_back_busy", 0) or busy
        sections = [
            self._sec_executive(ipc, synth, raw, busy, back_busy, total_vfs),
            self._sec_ipc(ipc),
            self._sec_topdown(td, nodes),
            self._sec_ub_bw(ub_bw),
            self._sec_port(raw),
            self._sec_util(util),
            self._sec_advanced(ipc, td, synth),
        ]
        body = "\n".join(sections)
        return (
            f'<!DOCTYPE html><html lang="en"><head>'
            f'<meta charset="UTF-8">'
            f'<meta name="viewport" '
            f'content="width=device-width,initial-scale=1.0">'
            f"<title>SIMD V100 Performance Summary Report</title>"
            f"{_CSS}{_TD_TREE_JS}</head><body>"
            f'<div class="container">'
            f"<h1>SIMD V100 Performance Summary Report &mdash; "
            f"Core {core_id} ({ct_name})</h1>"
            f"{body}</div></body></html>"
        )

    def _sec_executive(
        self,
        ipc: Dict,
        synth: Dict,
        raw: Dict,
        busy: float,
        back_busy: float,
        total_vfs: int,
    ) -> str:
        sim_ipc = ipc["Total"]["sim"]
        vec_ipc = ipc["Total"]["sim"] * (back_busy / max(busy, 1))
        total_instr = int(raw.get("c_slot_idu_disp_total", 0))

        cards = [
            ("Total IPC (sim)", _fmt(sim_ipc), ""),
            ("Total IPC (vec)", _fmt(vec_ipc), ""),
            ("Total Cycles", f"{int(busy):,}", ""),
            ("Total Instructions", f"{total_instr:,}", ""),
            ("Total VFs", f"{total_vfs:,}", ""),
        ]
        grid = []
        for label, value, cls in cards:
            card_cls = f"kpi-card {cls}".strip() if cls else "kpi-card"
            style = ' style="font-size:1.1em"' if cls == "warn" else ""
            grid.append(
                f'<div class="{card_cls}">'
                f'<div class="kpi-label">{label}</div>'
                f'<div class="kpi-value"{style}>{value}</div></div>'
            )

        note = (
            "Total IPC (sim) = c_slot_idu_disp_total / c_cycle_rvec_busy\n"
            "Total IPC (vec) = Total IPC (sim) x "
            "(c_cycle_rvec_back_busy / c_cycle_rvec_busy)\n"
            "Total Cycles = c_cycle_rvec_busy\n"
            "Total Instructions = c_slot_idu_disp_total\n"
            "Total VFs = count of VFs that dispatched on this subcore"
        )
        return _section(
            "1. Executive Summary",
            f'<div class="dashboard">{"".join(grid)}</div>',
            note,
        )

    def _sec_ipc(self, ipc: Dict) -> str:
        header = (
            "<tr><th>Type</th><th>IPC (sim)</th><th>Peak IPC</th>"
            "<th>Efficiency</th><th>Instr Count</th>"
            "<th>Instr Ratio</th></tr>"
        )
        rows = []
        for name in ["EX", "LD", "ST", "GA", "SC", "SU", "LP", "Total"]:
            d = ipc[name]
            sim = d["sim"]  # synthetic (syn_c_ipc_*)
            peak = d["peak"]  # ISU constant
            eff = sim / peak if peak > 0 else 0
            eff_pct = _pct(eff)
            gap = _pct(1 - eff)
            slots = int(d["slots"])  # raw slot count
            ratio = _pct(d["ratio"])  # synthetic (syn_c_ratio_*)
            bar_w = min(eff * 100, 100)
            color = _heat(eff)
            label = f"{name} (Compute)" if name == "EX" else name
            if name == "Total":
                label = "<strong>Total</strong>"
            sim_bar = _bar(bar_w, "#4caf50", 200)
            rows.append(
                f'<tr><td class="label-cell">{label}</td>'
                f"<td>{sim_bar} {_fmt(sim)}</td>"
                f"<td>{_fmt(peak)}</td>"
                f'<td class="eff-cell">'
                f'<span style="color:{color}">{eff_pct}</span>'
                f' <span style="color:#999;font-size:.85em">'
                f"(gap: {gap})</span></td>"
                f"<td>{slots:,}</td><td>{ratio}</td></tr>"
            )

        legend = (
            '<span class="legend-item"><span class="legend-color" '
            'style="background:#4caf50"></span> IPC (sim)</span>'
        )
        body = (
            f'<table class="data-table"><thead>{header}</thead>'
            f"<tbody>{''.join(rows)}</tbody></table>"
            f'<div class="legend">{legend}</div>'
        )
        return _section("2. IPC Breakdown Comparison", body)

    def _sec_topdown(self, td: Dict, nodes: List[dict]) -> str:
        colors = [_TD_L1_COLORS[n] for n in _L1_NAMES]
        vals = [td.get(n, 0) for n in _L1_NAMES]

        bar_parts = []
        for n, v, c in zip(_L1_NAMES, vals, colors):
            w = min(v * 100, 100)
            pct = _pct(v)
            label = n.replace("_", " ").title()
            bar_parts.append(
                f'<div style="display:inline-block;width:{w:.1f}%;'
                f'height:100%;background:{c};position:relative;" '
                f'title="{label}: {pct}">'
                f'<span style="color:#fff;font-size:.8em;font-weight:600;'
                f'line-height:30px;white-space:nowrap;padding:0 8px;">'
                f"{label} {pct}</span></div>"
            )

        waterfall = (
            f'<div style="width:100%;height:30px;background:#eee;'
            f'border-radius:4px;overflow:hidden;display:flex;">'
            f"{''.join(bar_parts)}</div>"
        )

        legend_parts = [
            f'<span class="legend-item"><span class="legend-color" '
            f'style="background:{c}"></span> '
            f"{n.replace('_', ' ').title()}</span>"
            for n, c in zip(_L1_NAMES, colors)
        ]

        tree_html = _build_td_tree_html(nodes)

        body = (
            f'<h3 style="font-size:1em;color:#555;margin-bottom:8px;">'
            f"Cycle Distribution Overview</h3>{waterfall}"
            f'<div class="legend" '
            f'style="margin-top:8px;margin-bottom:20px;">'
            f"{''.join(legend_parts)}</div>"
            f'<h3 style="font-size:1em;color:#555;margin-bottom:8px;">'
            f"Hierarchical Breakdown</h3>"
            f'<table class="data-table"><thead>'
            f'<tr><th style="width:40%">Metric</th>'
            f'<th style="width:40%">Distribution</th>'
            f'<th style="width:20%">Percentage</th></tr></thead>'
            f"<tbody>{tree_html}</tbody></table>"
        )
        return _section(
            "3. Top-Down Analysis Waterfall",
            body,
            "Read from the analyzer's per-subcore table.\n"
            "backend_vec_bound is decomposed twice: once by "
            "hardware resource and once by instruction type, so "
            "its children sum to it twice over.",
        )

    def _sec_ub_bw(self, ub_bw: Dict) -> str:
        header = (
            "<tr><th>Port Type</th><th>UB BW (sim, B/cyc)</th>"
            "<th>Peak BW</th><th>Utilization</th></tr>"
        )
        rows = []
        for port, d in ub_bw.items():
            sim = d["sim"]
            est = d["est"]
            ratio = sim / est if est > 0 else 0
            ratio_pct = _pct(ratio)
            color = _heat(ratio, 0.3, 0.6)
            bar_w = min(ratio * 100, 100)
            rows.append(
                f'<tr><td class="label-cell">ub_bw_{port}</td>'
                f"<td>{_bar(bar_w, '#4caf50', 160)} {_fmt(sim, 1)}</td>"
                f"<td>{_fmt(est, 1)}</td>"
                f'<td style="color:{color};font-weight:600;">'
                f"{ratio_pct}</td></tr>"
            )

        body = (
            f'<table class="data-table"><thead>{header}</thead>'
            f"<tbody>{''.join(rows)}</tbody></table>"
        )
        return _section(
            "4. Memory Bandwidth Analysis",
            body,
            "sim.ub_bw = grant_size / ub_port_busy",
        )

    def _sec_port(self, raw: Dict) -> str:
        ports = [
            (
                "LDU",
                "c_stat_ub_ldu_busy",
                "c_stat_ldu_grant_size",
                "c_stat_ldu_blocked",
            ),
            (
                "STU",
                "c_stat_ub_stu_busy",
                "c_stat_stu_grant_size",
                "c_stat_stu_blocked",
            ),
            ("GSU", "c_stat_ub_gsu_busy", "c_stat_gsu_grant_size", None),
        ]
        header = (
            "<tr><th>Unit</th><th>Port Busy (cycles)</th>"
            "<th>Grant Size (bytes)</th>"
            "<th>Blocked (cycles)</th>"
            "<th>Average (B/cyc)</th></tr>"
        )
        rows = []
        for name, busy_k, grant_k, block_k in ports:
            busy_v = int(raw.get(busy_k, 0))
            grant_v = int(raw.get(grant_k, 0))
            block_v = int(raw.get(block_k, 0)) if block_k else 0
            avg = grant_v / busy_v if busy_v > 0 else 0
            avg_bar = _bar(min(avg / 512 * 100, 100), _heat(avg / 512), 80)
            rows.append(
                f'<tr><td class="label-cell">{name}</td>'
                f"<td>{busy_v:,}</td><td>{grant_v:,}</td>"
                f"<td>{block_v:,}</td>"
                f"<td>{avg_bar} {_fmt(avg, 1)}</td></tr>"
            )

        body = (
            f'<table class="data-table"><thead>{header}</thead>'
            f"<tbody>{''.join(rows)}</tbody></table>"
        )
        return _section("5. Port Analysis", body)

    def _sec_util(self, util: Dict) -> str:
        header = (
            '<tr><th style="width:20%">Resource</th>'
            '<th style="width:65%">Utilization</th>'
            '<th style="width:15%">Value</th></tr>'
        )
        rows = []
        for name, v in util.items():
            pct_v = min(v * 100, 100)
            color = _heat(v)
            rows.append(
                f'<tr><td class="label-cell">{name}</td>'
                f"<td>{_bar(pct_v, color, 250)}</td>"
                f'<td style="font-weight:600;">{_fmt(v, 3)}</td></tr>'
            )

        body = (
            f'<table class="data-table"><thead>{header}</thead>'
            f"<tbody>{''.join(rows)}</tbody></table>"
        )
        return _section(
            "6. Queue & Register Utilization", body, "usage = used_sum / busy_cycles"
        )

    def _sec_advanced(self, ipc: Dict, td: Dict, synth: Dict) -> str:
        cards = []

        sim_total = ipc["Total"]["sim"]
        sim_eff = sim_total / IDU_WIDTH
        sim_eff_pct = _pct(sim_eff)
        gap_pct = _pct(1 - sim_eff)
        sim_w = min(sim_eff * 100, 100)
        gap_w = min((1 - sim_eff) * 100, 100)

        ipc_gap = (
            f'<div class="analysis-card full-width">'
            f"<h3>IPC Efficiency Gap Analysis</h3>"
            f'<div class="finding"><strong>Finding:</strong> '
            f"sim efficiency = {sim_eff_pct} (of peak {IDU_WIDTH}), "
            f"gap = {gap_pct}.</div>"
            f'<div style="width:100%;height:30px;background:#eee;'
            f'border-radius:4px;display:flex;overflow:hidden;margin:10px 0;">'
            f'<div style="width:{sim_w:.1f}%;height:100%;background:#4caf50;"'
            f' title="Achieved"></div>'
            f'<div style="width:{gap_w:.1f}%;height:100%;background:#f44336;"'
            f' title="Gap"></div></div>'
            f'<div class="legend">'
            f'<span class="legend-item"><span class="legend-color" '
            f'style="background:#4caf50"></span>'
            f" Achieved ({sim_eff_pct})</span>"
            f'<span class="legend-item"><span class="legend-color" '
            f'style="background:#f44336"></span>'
            f" Gap ({gap_pct})</span></div></div>"
        )
        cards.append(ipc_gap)

        vreg_stall = td.get("phy_vreg_stall", 0)
        dom_bound = max(((n, td.get(n, 0)) for n in _L1_NAMES), key=lambda x: x[1])
        dom_label = dom_bound[0].replace("_", " ").title()

        verdict = (
            '<div class="analysis-card full-width">'
            "<h3>Dominant Bottleneck</h3>"
            f'<div class="insight"><strong>{dom_label}</strong> '
            f"at {_pct(dom_bound[1])} of total slots."
        )
        if vreg_stall > 0.05:
            verdict += (
                f" VREG pressure: {_pct(vreg_stall)} &mdash; "
                f"recommend investigating vector register "
                f"allocation."
            )
        verdict += "</div></div>"
        cards.append(verdict)

        body = f'<div class="analysis-grid">{"".join(cards)}</div>'
        return _section("7. Advanced Analysis", body)

    def process(self) -> None:
        with StageReporter("[PMU] Performance summary report generation"):
            self._process()
        final_logger.add_general_info(f"[PMU] Perf summary reports: {self.output_dir}")


_assert_topdown_names(
    set(_L1_NAMES) | {"phy_vreg_stall"}, "vf_pmu_perf_summary_reporter"
)
