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

import html as html_mod
from typing import Dict


def _heat_cls(val: float, threshold: float = 0.10) -> str:
    if val < threshold * 0.3:
        return "c-green"
    if val < threshold * 0.6:
        return "c-yellow"
    if val < threshold * 0.85:
        return "c-orange"
    return "c-red"


def _fmt(val: float) -> str:
    return f"{val * 100:.1f}%"


def _tree_line(
    name: str, val: float, indent: int, is_last: bool, warn: bool = False
) -> str:
    cls = _heat_cls(val)
    conn = "\u2514" if is_last else "\u251c"
    pad = max(1, 28 - len(name) - indent * 2)
    w = " \u26a0" if warn else ""
    return (
        f'<div class="tree-line">'
        f'{"  " * indent}<span class="c-muted">{conn}</span> '
        f"{html_mod.escape(name)} {'.' * pad} "
        f'<span class="{cls} val">{_fmt(val)}</span>{w}</div>'
    )


def build_bottleneck_html(
    kernel_name: str,
    busy: int,
    back_busy: int,
    td: Dict[str, float],
    svg_inline: str,
) -> str:
    back_ratio: float = back_busy / max(busy, 1)
    m: Dict[str, float] = td

    def _badge(cls: str, text: str) -> str:
        return f'<span class="badge {cls}"><span class="badge-dot"></span>{text}</span>'

    major_bound: tuple = max(
        ("retired", m.get("retired", 0)),
        ("frontend_bound", m.get("frontend_bound", 0)),
        ("backend_asu_bound", m.get("backend_asu_bound", 0)),
        ("backend_vec_bound", m.get("backend_vec_bound", 0)),
        key=lambda x: x[1],
    )
    mc: str = "badge-red" if major_bound[1] > 0.3 else "badge-orange"
    rc: str = "badge-red" if m.get("phy_vreg_stall", 0) > 0.2 else "badge-orange"

    tree_parts: list = []
    ret: float = m.get("retired", 0)
    tree_parts.append(
        f'<div class="tree-line"><span class="{_heat_cls(ret)} val">'
        f"retired {'.' * 24} {_fmt(ret)}</span></div>"
    )
    for short in ["rvecex", "rvecld", "rvecst", "rvecsu", "rveclp"]:
        k: str = f"retired_{short}"
        v: float = m.get(k, 0)
        last: bool = k == "retired_rveclp"
        conn: str = "&#x2514;" if last else "&#x251C;"
        tree_parts.append(
            f'<div class="tree-line">  '
            f'<span class="c-muted">{conn}</span> '
            f"{short:>5s} {'.' * (24 - len(short))} "
            f'<span class="{_heat_cls(v)} val">{_fmt(v)}</span></div>'
        )

    fb: float = m.get("frontend_bound", 0)
    tree_parts.append(
        f'<div class="tree-line" style="margin-top:4px">'
        f'<span class="{_heat_cls(fb)} val">frontend_bound '
        f"{'.' * 14} {_fmt(fb)}</span></div>"
    )
    tree_parts.append(_tree_line("fetch_latency", m.get("fetch_latency", 0), 1, False))
    tree_parts.append(
        _tree_line("fetch_bandwidth", m.get("fetch_bandwidth", 0), 1, True)
    )
    tree_parts.extend(
        _tree_line(k, m.get(k, 0), 2, k == "idu_ports_stall")
        for k in [
            "idu_fifo_stall",
            "idu_sreg_stall",
            "idu_instr_stall",
            "idu_ports_stall",
        ]
    )

    bvb: float = m.get("backend_vec_bound", 0)
    tree_parts.append(
        f'<div class="tree-line" style="margin-top:4px">'
        f'<span class="{_heat_cls(bvb)} val">backend_vec_bound '
        f"{'.' * 12} {_fmt(bvb)}</span></div>"
    )
    for k in [
        "phy_reg_stall",
        "issue_que_stall",
        "send_stall",
        "ld_issue_stall",
        "ex_issue_stall",
        "st_issue_stall",
    ]:
        v = m.get(k, 0)
        w: bool = k == "phy_vreg_stall" and v > 0.30
        tree_parts.append(_tree_line(k, v, 1, k == "st_issue_stall", w))
        if k == "phy_reg_stall":
            tree_parts.append(
                _tree_line("phy_preg_stall", m.get("phy_preg_stall", 0), 2, False)
            )
            tree_parts.append(
                _tree_line("phy_vreg_stall", m.get("phy_vreg_stall", 0), 2, True)
            )
        if k == "issue_que_stall":
            tree_parts.append(_tree_line("ldq_stall", m.get("ldq_stall", 0), 2, False))
            tree_parts.append(_tree_line("shq_stall", m.get("shq_stall", 0), 2, True))
        if k == "ex_issue_stall":
            tree_parts.extend(
                _tree_line(sk, m.get(sk, 0), 2, sk == "ex_stall_imbalance")
                for sk in [
                    "ex_stall_src_bound",
                    "ex_stall_dst_reg_port",
                    "ex_stall_valu_cflt",
                    "ex_stall_imbalance",
                ]
            )
        if k == "st_issue_stall":
            tree_parts.extend(
                _tree_line(sk, m.get(sk, 0), 2, sk == "st_stall_fu_cflt")
                for sk in [
                    "st_stall_src_bound",
                    "st_stall_fu_cflt",
                ]
            )
        if k == "ld_issue_stall":
            tree_parts.extend(
                _tree_line(sk, m.get(sk, 0), 2, sk == "ld_stall_others")
                for sk in [
                    "ld_stall_membar",
                    "ld_stall_fu_cflt",
                    "ld_stall_others",
                ]
            )

    asu_val: float = m.get("backend_asu_bound", 0)
    tree_parts.append(
        f'<div class="tree-line" style="margin-top:4px">'
        f'<span class="{_heat_cls(asu_val)} val">backend_asu_bound '
        f"{'.' * 12} {_fmt(asu_val)}</span></div>"
    )

    tree_html: str = "\n".join(tree_parts)

    vreg_pct: float = m.get("phy_vreg_stall", 0) * 100
    major_label: str = major_bound[0].replace("_", " ").title()
    subtitle: str = (
        f"{html_mod.escape(kernel_name)} &nbsp;|&nbsp; "
        f"Busy Cycles: {busy:,} &nbsp;|&nbsp; "
        f"Backend Busy: {back_ratio * 100:.1f}%"
    )
    major_text: str = f"MAJOR BOUND: {major_label} ({_pct(major_bound[1])})"
    resource_text: str = f"RESOURCE: {vreg_pct:.1f}% VREG"
    badges: str = _badge(mc, major_text) + _badge(rc, resource_text)

    page: str = f"""
<!DOCTYPE html>
<html lang="en">

<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>V100 Vector Core Bottleneck Visualization</title>
    <style>
        *,
        *::before,
        *::after {{
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }}

        body {{
            font-family: system-ui, -apple-system, sans-serif;
            background: #f8f9fa;
            color: #1e293b;
            min-height: 100vh;
            overflow-x: auto;
        }}

        .header {{
            background: linear-gradient(135deg, #1e293b 0%, #334155 100%);
            color: #fff;
            padding: 20px 32px 16px;
        }}

        .header h1 {{
            font-size: 22px;
            font-weight: 700;
            letter-spacing: -0.5px;
        }}

        .header .subtitle {{
            font-size: 13px;
            color: #94a3b8;
            margin-top: 4px;
            font-family: 'SF Mono', 'Cascadia Code', 'Fira Code', monospace;
        }}

        .badges {{
            display: flex;
            flex-wrap: wrap;
            gap: 8px;
            margin-top: 12px;
        }}

        .badge {{
            display: inline-flex;
            align-items: center;
            gap: 6px;
            padding: 5px 12px;
            border-radius: 20px;
            font-size: 12px;
            font-weight: 600;
            border: 1px solid transparent;
        }}

        .badge-red {{
            background: rgba(239, 68, 68, 0.15);
            color: #fca5a5;
            border-color: rgba(239, 68, 68, 0.3);
        }}

        .badge-orange {{
            background: rgba(249, 115, 22, 0.15);
            color: #fdba74;
            border-color: rgba(249, 115, 22, 0.3);
        }}

        .badge-dot {{
            width: 7px;
            height: 7px;
            border-radius: 50%;
        }}

        .badge-red .badge-dot {{
            background: #ef4444;
        }}

        .badge-orange .badge-dot {{
            background: #f97316;
        }}

        .main-layout {{
            display: flex;
            gap: 0;
            max-width: 1600px;
            margin: 0 auto;
        }}

        .svg-container {{
            flex: 1;
            min-width: 0;
            padding: 16px;
            overflow-x: auto;
        }}

        .svg-container object {{
            width: 100%;
            min-width: 1000px;
            height: auto;
        }}

        .svg-container svg {{
            width: 100%;
            min-width: 900px;
            height: auto;
        }}

        .sidebar {{
            width: 310px;
            min-width: 310px;
            background: #fff;
            border-left: 1px solid #e2e8f0;
            padding: 16px 14px;
            font-size: 11.5px;
            line-height: 1.6;
        }}

        .sidebar h3 {{
            font-size: 13px;
            font-weight: 700;
            margin-bottom: 10px;
            color: #475569;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }}

        .tree-line {{
            white-space: pre;
            font-family: 'SF Mono', 'Cascadia Code', 'Fira Code', monospace;
            font-size: 10.5px;
            line-height: 1.7;
        }}

        .tree-line .val {{
            font-weight: 600;
        }}

        .c-green {{
            color: #16a34a;
        }}

        .c-yellow {{
            color: #ca8a04;
        }}

        .c-orange {{
            color: #ea580c;
        }}

        .c-red {{
            color: #dc2626;
            font-weight: 700;
        }}

        .c-muted {{
            color: #94a3b8;
        }}
    </style>
</head>

<body>
    <div class="header">
        <h1>V100 Vector Core Bottleneck Visualization</h1>
        <div class="subtitle">{subtitle}</div>
        <div class="badges">{badges}</div>
    </div>
    <div class="main-layout">
        <div class="svg-container">
            {svg_inline}
        </div>
        <div class="sidebar" id="sidebar">
            <h3>Top-Down Breakdown</h3> {tree_html}
        </div>
    </div>
</body>
</html>"""
    return page


def _pct(val: float) -> str:
    return f"{val * 100:.1f}%"
