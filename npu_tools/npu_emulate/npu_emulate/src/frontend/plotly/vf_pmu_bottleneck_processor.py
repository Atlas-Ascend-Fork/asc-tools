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
import xml.etree.ElementTree as ET
from typing import Dict, List, Optional

NS_SVG = "{http://www.w3.org/2000/svg}"


BLOCK_METRICS: Dict[str, str] = {
    "block_ifu": "fetch_latency",
    "block_idu": "fetch_bandwidth",
    "block_ldq": "ldq_stall",
    "block_shq": "shq_stall",
    "block_vreg": "phy_vreg_stall",
    "block_preg": "phy_preg_stall",
    "block_vec_exu": "ex_issue_stall",
    "block_ldu": "ld_issue_stall",
    "block_stu": "st_issue_stall",
}

DEP_GROUPS: Dict[str, str] = {
    "group_dep_ld_to_ex": "ex_stall_by_ld",
    "group_dep_vreg_to_exq": "phy_vreg_stall",
    "group_dep_ex_to_stu": "st_stall_src_bound",
}


def _heat_color(value: float, threshold: float = 0.10) -> str:
    if value < threshold * 0.3:
        return "#22c55e"
    if value < threshold * 0.6:
        return "#eab308"
    if value < threshold * 0.85:
        return "#f97316"
    return "#ef4444"


def _heat_stroke(value: float, threshold: float = 0.10) -> str:
    c = _heat_color(value, threshold)
    return {
        "#22c55e": "#16a34a",
        "#eab308": "#ca8a04",
        "#f97316": "#ea580c",
        "#ef4444": "#dc2626",
    }[c]


def _find_by_tag(node: ET.Element, tag: str, result: list) -> None:
    for item in node:
        parts = item.tag.split("}")
        real_tag = parts[-1] if len(parts) > 1 else parts[0]
        if real_tag == tag:
            result.append(item)
        _find_by_tag(item, tag, result)


class BottleneckProcessor:
    def __init__(
        self, template_path: str, output_path: str, metrics: Dict[str, float]
    ) -> None:
        self._template = template_path
        self._output = output_path
        self._metrics = metrics
        self._tree = ET.parse(self._template)
        self._root = self._tree.getroot()

    def patch(self) -> int:
        self._patch_tspan_values()
        self._patch_block_colors()
        self._patch_dep_arrows()
        self._write()
        return 0

    def patch_to_string(self) -> str:
        self._patch_tspan_values()
        self._patch_block_colors()
        self._patch_dep_arrows()
        ET.register_namespace("", "http://www.w3.org/2000/svg")
        return ET.tostring(self._root, encoding="unicode", method="xml")

    def _patch_tspan_values(self) -> None:
        tspans: List[ET.Element] = []
        _find_by_tag(self._root, "tspan", tspans)
        for ts in tspans:
            tid = ts.get("id", "")
            val = self._resolve_tspan_id(tid)
            if val is not None:
                ts.text = val

    def _resolve_tspan_id(self, tid: str) -> Optional[str]:
        if not tid:
            return None
        if tid.endswith("_dep_pct"):
            key = tid[:-8]
            v = self._metrics.get(key)
            if v is not None:
                return f"{v * 100:.1f}%"
        elif tid.endswith("_pct"):
            key = tid[:-4]
            v = self._metrics.get(key)
            if v is not None:
                return f"{v * 100:.1f}%"
        elif tid.endswith("_val"):
            key = tid[:-4]
            v = self._metrics.get(key)
            if v is not None:
                return f"{v:.1f}"
        return None

    def _patch_block_colors(self) -> None:
        g_nodes: List[ET.Element] = []
        _find_by_tag(self._root, "g", g_nodes)
        for g in g_nodes:
            gid = g.get("id", "")
            if not gid.startswith("block_"):
                continue
            metric_name = BLOCK_METRICS.get(gid)
            if metric_name is None:
                continue
            if metric_name == "__ub_bw_ratio__":
                value = self._metrics.get("ub_bw_ratio", 0.0)
                threshold = 0.5
            elif metric_name == "__ga_active__":
                value = 0.0
                threshold = 0.10
            else:
                value = self._metrics.get(metric_name, 0.0)
                threshold = 0.10
            fill = _heat_color(value, threshold)
            stroke = _heat_stroke(value, threshold)
            for rect in g.findall(f".//{NS_SVG}rect") or _find_direct_rect(g):
                if "block-shape" in (rect.get("class") or ""):
                    rect.set("fill", fill)
                    rect.set("stroke", stroke)
            if value > 0.30:
                self._apply_glow(g)

    def _patch_dep_arrows(self) -> None:
        g_nodes: List[ET.Element] = []
        _find_by_tag(self._root, "g", g_nodes)
        for g in g_nodes:
            gid = g.get("id", "")
            if not gid.startswith("group_dep_"):
                continue
            metric_name = DEP_GROUPS.get(gid)
            if metric_name is None:
                continue
            value = self._metrics.get(metric_name, 0.0)
            if value > 0.005:
                color = _heat_color(value, 0.01)
                for tag_name in ("path", "line"):
                    for elem in _find_by_tag_return(g, tag_name):
                        elem.set("stroke", color)
                for text in _find_by_tag_return(g, "text"):
                    text.set("fill", color)

    def _apply_glow(self, g: ET.Element) -> None:
        for rect in _find_by_tag_return(g, "rect"):
            cls = rect.get("class", "")
            if "block-shape" in cls:
                rect.set("filter", "url(#glowRed)")

    def _write(self) -> None:
        self._tree.write(self._output, xml_declaration=True, encoding="unicode")


def _find_direct_rect(parent: ET.Element) -> list:
    result = []
    for child in parent:
        tag = child.tag.split("}")[-1]
        if tag == "rect":
            result.append(child)
    return result


def _find_by_tag_return(parent: ET.Element, tag: str) -> list:
    result: List[ET.Element] = []
    _find_by_tag(parent, tag, result)
    return result


def get_template_path() -> str:
    from npu_emulate.src.common.utils import (
        get_pyinstaller_path,
        is_in_pyinstaller_env,
    )

    if is_in_pyinstaller_env():
        base_path = get_pyinstaller_path()
    else:
        base_path = os.path.join(os.path.dirname(__file__), "..", "..", "..")
    return os.path.abspath(
        os.path.join(
            base_path, "static", "top_down", "v100", "bottleneck_pipeline_view.svg"
        )
    )
