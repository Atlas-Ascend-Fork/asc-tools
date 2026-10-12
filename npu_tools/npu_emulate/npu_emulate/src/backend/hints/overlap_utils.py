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

from collections import defaultdict
from typing import Callable, DefaultDict, Dict, List, Optional, Tuple

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_enum import CoreType, InstrType


def _merge_intervals(ivs: List[Tuple[int, int]]) -> List[Tuple[int, int]]:
    if not ivs:
        return []
    ivs = sorted(ivs)
    merged: List[List[int]] = [list(ivs[0])]
    for s, e in ivs[1:]:
        if s <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([s, e])
    return [(x[0], x[1]) for x in merged]


def _interval_length(ivs: List[Tuple[int, int]]) -> int:
    return sum(e - s for s, e in ivs)


def _intersect_intervals(
    a: List[Tuple[int, int]], b: List[Tuple[int, int]]
) -> List[Tuple[int, int]]:
    res, i, j = [], 0, 0
    while i < len(a) and j < len(b):
        lo, hi = max(a[i][0], b[j][0]), min(a[i][1], b[j][1])
        if lo < hi:
            res.append((lo, hi))
        if a[i][1] < b[j][1]:
            i += 1
        else:
            j += 1
    return res


def _union_intervals(
    a: List[Tuple[int, int]], b: List[Tuple[int, int]]
) -> List[Tuple[int, int]]:
    return _merge_intervals(a + b)


def compute_overlaps(
    pipe_pairs: List[Tuple[Tuple[str, str], Tuple[str, str], str]],
) -> Dict[str, Optional[float]]:
    rows = database_handler.execute(
        """
        SELECT ExecInstrCoreId, CoreTypeId, InstrTypeId,
               ExecInstrTickStart, ExecInstrTickEnd
        FROM ExecutedInstructions
        """
    ).fetchall()

    intervals: DefaultDict[int, DefaultDict[Tuple[str, str], List[Tuple[int, int]]]] = (
        defaultdict(lambda: defaultdict(list))
    )
    for r in rows:
        instr_type = InstrType(r["InstrTypeId"])
        pipe_name = "VEC" if instr_type.name == "PUSHQ" else instr_type.name
        core_type = CoreType(r["CoreTypeId"]).name
        key = (core_type, pipe_name)
        intervals[r["ExecInstrCoreId"]][key].append(
            (r["ExecInstrTickStart"], r["ExecInstrTickEnd"])
        )

    merged_by_core = {
        core_id: {key: _merge_intervals(ivs) for key, ivs in pipes.items()}
        for core_id, pipes in intervals.items()
    }

    result = {}
    for side_a, side_b, label in pipe_pairs:
        total_inter = 0
        total_union = 0
        for core_pipes in merged_by_core.values():
            ivs_a = core_pipes.get(side_a, [])
            ivs_b = core_pipes.get(side_b, [])
            if not ivs_a or not ivs_b:
                continue
            total_inter += _interval_length(_intersect_intervals(ivs_a, ivs_b))
            total_union += _interval_length(_union_intervals(ivs_a, ivs_b))
        result[label] = round(total_inter / total_union, 4) if total_union else None

    return result


def get_ai_core_active() -> int:
    row = database_handler.execute(
        """
        SELECT KernelInfoVal FROM KernelInfo
        WHERE KernelInfoAttr = 'ai_core_active'
        """
    ).fetchone()
    return int(row[0]) if row else 0


def all_overlaps_below(
    overlaps: Dict[str, Optional[float]],
    threshold: float,
    filter_fn: Optional[Callable[[str], bool]] = None,
) -> bool:
    if filter_fn is None:
        return True
    filtered = {k: v for k, v in overlaps.items() if filter_fn(k)}
    if not filtered:
        return True
    return all(v is not None and v < threshold for v in filtered.values())


def overlaps_to_string(
    overlaps: Dict[str, Optional[float]],
    filter_fn: Optional[Callable[[str], bool]] = None,
) -> str:
    if filter_fn is None:
        return "N/A"
    filtered = {k: v for k, v in overlaps.items() if filter_fn(k)}
    if not filtered:
        return "N/A"
    return ", ".join(f"{k}={v}" for k, v in sorted(filtered.items()))
