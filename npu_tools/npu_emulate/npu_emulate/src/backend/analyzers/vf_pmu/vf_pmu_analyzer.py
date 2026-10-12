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

"""VF PMU top-down analyzer: orchestration only.

Two passes over the same pure evaluate(): subcore (fed the last VF's
cumulative counters, == the sum of all per-VF deltas) and per VF (fed that
VF's deltas). Formulas live in metrics.py, SQL in storage.py, tree
rendering in render.py.
"""

import logging as log
from typing import Dict, List, Tuple

from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer
from npu_emulate.src.backend.analyzers.vf_pmu import storage
from npu_emulate.src.backend.analyzers.vf_pmu.metrics import (
    SUBCORE_REQUIRED,
    VF_METRICS,
    VF_REQUIRED,
    MetricScope,
    RawUsage,
    evaluate,
    values_for_scope,
)
from npu_emulate.src.backend.analyzers.vf_pmu.render import (
    render_counter_values,
    render_raw_values,
    render_topdown_tree,
)
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_enum import CoreType

# Only the hierarchy part of the per-VF metric list is drawn as a tree.
_VF_TOPDOWN_METRICS = [m for m in VF_METRICS if m.level >= 1]


def _debug_enabled() -> bool:
    return log.getLogger().isEnabledFor(log.DEBUG)


class VfPMUAnalyzer(IAnalyzer):
    @staticmethod
    def get_name() -> str:
        return "vf_pmu_topdown_analyzer"

    def _log_subcore(
        self,
        core_id: int,
        core_type_id: int,
        raw_used: Dict[str, RawUsage],
        values: Dict[str, float],
    ) -> None:
        if not _debug_enabled():
            return
        ct_name = CoreType(core_type_id).name
        log.debug(
            (
                "VF PMU raw counters used (core %s, %s):\n%s\n\n"
                "VF PMU synthetic counters (core %s, %s):\n%s\n\n"
                "VF PMU top-down derivation (core %s, %s):\n%s\n\n"
            ),
            core_id,
            ct_name,
            render_raw_values(raw_used),
            core_id,
            ct_name,
            render_counter_values(values),
            core_id,
            ct_name,
            render_topdown_tree(values),
        )

    def _log_vf(
        self,
        core_id: int,
        core_type_id: int,
        vf_id: int,
        raw_used: Dict[str, RawUsage],
        values: Dict[str, float],
    ) -> None:
        if not _debug_enabled():
            return
        ct_name = CoreType(core_type_id).name
        log.debug(
            (
                "VF PMU raw counters used (core %s, %s, VF %s):\n%s\n\n"
                "VF PMU synthetic counters (core %s, %s, VF %s):\n%s\n\n"
                "VF PMU per-VF top-down (core %s, %s, VF %s):\n%s\n\n"
            ),
            core_id,
            ct_name,
            vf_id,
            render_raw_values(raw_used),
            core_id,
            ct_name,
            vf_id,
            render_counter_values(values),
            core_id,
            ct_name,
            vf_id,
            render_topdown_tree(values, _VF_TOPDOWN_METRICS),
        )

    def _process_subcores(self, name_to_id: Dict[str, int]) -> None:
        subcores = storage.fetch_all_subcore_metrics()
        for (core_id, ct_id), raw in subcores.items():
            if not raw:
                continue
            raw_used, values = evaluate(raw, SUBCORE_REQUIRED)
            self._log_subcore(core_id, ct_id, raw_used, values)
            storage.save_subcore_synth_values(
                name_to_id,
                core_id,
                ct_id,
                values_for_scope(values, MetricScope.SUBCORE),
            )

    def _process_vfs(self, name_to_id: Dict[str, int]) -> None:
        # VF_REQUIRED is the transitive closure of the per-VF metrics: a
        # per-VF node may depend on a subcore-only intermediate, so the
        # closure is wider than the scope. values_for_scope() then trims the
        # intermediates back out before persisting.
        batch: List[Tuple[storage.VfKey, Dict[str, float]]] = []
        for key, delta in storage.fetch_all_vf_deltas().items():
            if not delta:
                continue
            core_id, ct_id, vf_id = key
            raw_used, values = evaluate(delta, VF_REQUIRED)
            vf_values = values_for_scope(values, MetricScope.VF)
            self._log_vf(core_id, ct_id, vf_id, raw_used, values)
            batch.append((key, vf_values))
        storage.save_vf_synth_values(name_to_id, batch)

    def process(self) -> None:
        name_to_id = storage.register_synth_types()
        storage.make_indexes()
        try:
            self._process_subcores(name_to_id)
            self._process_vfs(name_to_id)
            database_handler.commit()
        finally:
            storage.drop_indexes()
