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

"""Per-subcore top-down report: the bottleneck SVG.

No arithmetic lives here. Every number is a synthetic counter the analyzer
already computed and stored; one query per subcore pulls the whole
synthetic namespace, and the level >= 1 subset is the hierarchy.
"""

import os
from typing import Dict, List

from npu_emulate.src.backend.analyzers.vf_pmu import storage
from npu_emulate.src.backend.analyzers.vf_pmu.metrics import (
    ISU_MAX_EX,
    ISU_MAX_GA,
    ISU_MAX_LD,
    ISU_MAX_ST,
    SUBCORE_DISPLAY_NAMES,
    TOPDOWN_DISPLAY_NAMES,
    UB_PEAK_LSU,
    display_name,
)
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.common.biprof_enum import CoreType
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.frontend.base.plotly_reporter_base import (
    PlotlyReporterBase,
)
from npu_emulate.src.frontend.plotly.vf_pmu_bottleneck_processor import (
    BottleneckProcessor,
    get_template_path,
)
from npu_emulate.src.frontend.plotly.vf_pmu_bottleneck_viz import (
    build_bottleneck_html,
)

# Metric names below are DISPLAY names (no syn_c_ prefix). The assertions at
# the bottom guarantee every name really exists - otherwise a typo would
# silently plot 0.0.

# Queue / register occupancy metrics; the SVG template reads them by these
# exact names, which is why the analyzer publishes them as util_<name>.
_UTIL_NAMES = [
    "util_ldq",
    "util_shq",
    "util_exq0",
    "util_exq1",
    "util_vreg",
    "util_preg",
]

# The SVG template predates the metric names. Left side is the metric, right
# side the key(s) the template looks up. ub_bw_ratio and ub_ratio hold the
# same value - the template reads one of them; which one is not documented.
_SVG_ALIASES = {
    "ub_bw_lsu": ("ub_bw",),
    "ub_bw_lsu_ratio": ("ub_bw_ratio", "ub_ratio"),
}

# ISU peaks, echoed into the bottleneck page next to the achieved IPC.
_IPC_PORTS = {
    "ex": ISU_MAX_EX,
    "ld": ISU_MAX_LD,
    "ga": ISU_MAX_GA,
    "st": ISU_MAX_ST,
}


def _fetch_synth(core_id: int, core_type_id: int) -> Dict[str, float]:
    """{display_name: value} for every SYNTHETIC counter of one subcore.

    Keys are display names (syn_c_ prefix stripped) so nothing leaks into the
    report; the values are all synthetic, never raw.
    """
    values = storage.fetch_subcore_synth_values(core_id, core_type_id)
    return {display_name(name): value for name, value in values.items()}


class VfPMUTopDownReporter(PlotlyReporterBase):
    def _svg_metrics(
        self, synth: Dict[str, float], td_agg: Dict[str, float]
    ) -> Dict[str, float]:
        """Namespace handed to the SVG template: hierarchy + named ratios.

        Every value here is synthetic (td_agg is the hierarchy subset, the
        rest are read from `synth`); the template keys stay display-named.
        """
        metrics = dict(td_agg)
        for name in _UTIL_NAMES:
            metrics[name] = synth.get(name, 0.0)
        for metric, aliases in _SVG_ALIASES.items():
            for alias in aliases:
                metrics[alias] = synth.get(metric, 0.0)
        return metrics

    def _generate_bottleneck_viz(
        self,
        core_id: int,
        core_type_id: int,
        ct_name: str,
        synth: Dict[str, float],
        td_agg: Dict[str, float],
        busy: int,
        back_busy: int,
    ) -> None:
        template_path = get_template_path()
        svg_inline = BottleneckProcessor(
            template_path, "", self._svg_metrics(synth, td_agg)
        ).patch_to_string()

        page = build_bottleneck_html(
            kernel_name=f"core_{core_id}_{ct_name}",
            busy=busy,
            back_busy=back_busy,
            td=td_agg,
            svg_inline=svg_inline,
        )
        html_name = f"pmu_bottleneck_core_{core_id}_{ct_name}.html"
        html_path = os.path.join(self.output_dir, html_name)
        with open(html_path, "w") as f:
            f.write(page)

    def _process_core(self, core_id: int, core_types: List[int]) -> None:
        processed = False
        for core_type_id in core_types:
            ct_name = CoreType(core_type_id).name
            synth = _fetch_synth(core_id, core_type_id)  # synthetic counters
            if not synth:
                continue

            # Busy cycle counts are raw, not synthetic: read them straight from
            # the raw per-subcore view rather than republishing them as synth.
            raw = storage.fetch_subcore_metrics(
                core_id, core_type_id, ["c_cycle_rvec_busy", "c_cycle_rvec_back_busy"]
            )
            busy = int(raw.get("c_cycle_rvec_busy", 0))
            back_busy = int(raw.get("c_cycle_rvec_back_busy", 0))

            # Synthetic hierarchy subset (level >= 1).
            td_agg = {
                name: value
                for name, value in synth.items()
                if name in TOPDOWN_DISPLAY_NAMES
            }
            processed = True
            self._generate_bottleneck_viz(
                core_id, core_type_id, ct_name, synth, td_agg, busy, back_busy
            )

        if not processed:
            final_logger.add_general_info(
                f"[PMU] core {core_id}: no synthetic counters; has the analyzer run?"
            )

    def _process(self) -> None:
        cores = BackendDBManager.get_active_cores()
        core_types = BackendDBManager.get_active_core_types_ordered()

        storage.make_indexes()
        try:
            for core_id in cores:
                self._process_core(core_id, core_types)
        finally:
            storage.drop_indexes()

    def process(self) -> None:
        with StageReporter("[PMU] Top-down analysis report generation"):
            self._process()
        final_logger.add_general_info(f"[PMU] Top-down reports: {self.output_dir}")


# busy / back_busy are raw cycle counters (read from the raw view), so they
# are deliberately absent here: this set only covers the synthetic counters.
_PLAIN_NAMES = (
    set(_UTIL_NAMES)
    | set(_SVG_ALIASES)
    | {f"ipc_back_{p}" for p in _IPC_PORTS}
    | {f"ipc_back_{p}_ratio" for p in _IPC_PORTS}
)
_UNKNOWN_PLAIN = _PLAIN_NAMES - SUBCORE_DISPLAY_NAMES
assert not _UNKNOWN_PLAIN, (
    f"reporter reads counters the analyzer does not persist per subcore: "
    f"{sorted(_UNKNOWN_PLAIN)}"
)

assert UB_PEAK_LSU > 0, "UB_PEAK_LSU must be positive"
