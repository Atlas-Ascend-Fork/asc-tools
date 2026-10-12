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

"""Shared building blocks for the two Perf Summary reports.

vf_pmu_perf_summary_reporter.py (one page per subcore) and
vf_pmu_perf_per_vf_reporter.py (one page per subcore, one card/row per VF)
render two different pages off the same synthetic top-down hierarchy, and
share enough small formatting helpers, the L1 metric names/colors, and the
ISU/IDU peak constants that keeping two copies invited drift - this file
used to duplicate IDU_WIDTH/ISU_MAX_* as local literals instead of the ones
already defined in metrics.py. Both reporters import from here instead of
redefining any of this.

Pure module: HTML-fragment formatting only, no DB access.
"""

import html as html_mod
from typing import Set

from npu_emulate.src.backend.analyzers.vf_pmu.metrics import (
    IDU_WIDTH,
    ISU_MAX_EX,
    ISU_MAX_GA,
    ISU_MAX_LD,
    ISU_MAX_ST,
    TOPDOWN_DISPLAY_NAMES,
)

# Re-exported so both reporters get the peak/width constants from one place
# (metrics.py) instead of each hand-copying them. Referenced via this module
# rather than importing metrics directly so a future constant that's specific
# to report *rendering* (not present in metrics.py) has an obvious home.
__all__ = [
    "IDU_WIDTH",
    "ISU_MAX_EX",
    "ISU_MAX_GA",
    "ISU_MAX_LD",
    "ISU_MAX_ST",
    "_L1_NAMES",
    "_TD_L1_COLORS",
    "_assert_topdown_names",
    "_bar",
    "_fmt",
    "_heat",
    "_pct",
    "_section",
]

# Display names (no syn_c_ prefix) of the four top-down roots.
_L1_NAMES = ["retired", "frontend_bound", "backend_vec_bound", "backend_asu_bound"]

_TD_L1_COLORS = {
    "retired": "#4caf50",
    "frontend_bound": "#2196f3",
    "backend_asu_bound": "#ff9800",
    "backend_vec_bound": "#f44336",
}


def _bar(
    width_pct: float,
    color: str,
    width_px: int = 200,
    show_pct: bool = False,
    val_str: str = "",
) -> str:
    fill = f"width:{width_pct:.1f}%;background:{color}"
    inner = f'<div class="bar-fill" style="{fill}"></div>'
    if show_pct and val_str:
        inner += f'<div class="bar-label">{val_str}</div>'
    return f'<div class="bar" style="width:{width_px}px">{inner}</div>'


def _section(
    title: str, body_html: str, note: str = "", collapsed: bool = False
) -> str:
    note_html = ""
    if note:
        note_html = (
            f'<div class="rpt-note"><div class="rpt-note-title">'
            f"Source Notes</div>{html_mod.escape(note)}</div>"
        )
    open_attr = "" if collapsed else " open"
    return (
        f"<details{open_attr}>"
        f"<summary><h2>{title}</h2></summary>"
        f'<div class="collapsible-body">'
        f"{body_html}{note_html}</div></details>"
    )


def _fmt(v: float, decimals: int = 3) -> str:
    return f"{v:.{decimals}f}"


def _pct(v: float) -> str:
    return f"{v * 100:.1f}%"


def _heat(v: float, lo: float = 0.3, hi: float = 0.7) -> str:
    if v < lo:
        return "#4caf50"
    if v < hi:
        return "#ff9800"
    return "#f44336"


def _assert_topdown_names(names: Set[str], source: str) -> None:
    """Fail fast at import rather than silently plotting 0.0 for a top-down
    metric name that was renamed or removed in the analyzer.

    Each reporter calls this once, at module scope, with exactly the literal
    top-down names it references - so a name only one of the two reports
    still uses doesn't have to be tracked by the other.
    """
    unknown = names - TOPDOWN_DISPLAY_NAMES
    assert not unknown, (
        f"{source}: references non-existent top-down metrics: {sorted(unknown)}"
    )
