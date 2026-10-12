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
import plotly.graph_objects as go  # type: ignore
from typing import Any, Dict, List, Optional
from npu_emulate.src.common.biprof_enum import CoreType
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.frontend.base.plotly_reporter_base import PlotlyReporterBase


class DataCacheUtilizationReporter(PlotlyReporterBase):
    @staticmethod
    def get_events(
        core_id: int, core_type_id: int, hit_status: bool
    ) -> Dict[str, List[Any]]:
        rows = database_handler.execute(
            """
            SELECT SourceInstructions.SourceInstrText AS InstrText,
                DCacheHitMissEvents.CacheLineAddress AS Addr,
                DCacheHitMissEvents.CoreId AS CoreId,
                DCacheHitMissEvents.CoreTypeId AS Subcore,
                DCacheHitMissEvents.IsHit AS IsHit,
                DCacheHitMissEvents.EventTick AS Tick
            FROM DCacheHitMissEvents
            LEFT JOIN SourceInstructions
            ON DCacheHitMissEvents.SourceInstrAddr=
                SourceInstructions.SourceInstrAddr
            WHERE CoreId=:core_id AND
                  Subcore=:core_type_id AND
                  IsHit=:hs
            ORDER BY Tick;
        """,
            {"core_id": core_id, "core_type_id": core_type_id, "hs": hit_status},
        ).fetchall()
        frame = {
            "Tick": [r["Tick"] for r in rows],
            "Addr": [hex(r["Addr"]) for r in rows],
            "CustomData": [
                f"InstrText: {r['InstrText']}<br>Addr: {hex(r['Addr'])}<br>"
                for r in rows
            ],
        }
        return frame

    def generate_subreport(
        self, core_id: int, core_type_id: int
    ) -> Optional[go.Figure]:
        all_addr: List[Any] = []
        hits_len = 0
        misses_len = 0
        graph = go.Figure()
        hits = self.get_events(core_id, core_type_id, True)
        misses = self.get_events(core_id, core_type_id, False)
        all_addr += hits["Addr"]
        all_addr += misses["Addr"]
        hits_len += len(hits["Addr"])
        misses_len += len(misses["Addr"])

        graph.add_trace(
            go.Scatter(
                x=hits["Tick"],
                y=hits["Addr"],
                name="HIT",
                legendgrouptitle_text="DCACHE",
                customdata=hits["CustomData"],
                marker_color="blue",
                marker_symbol="diamond",
                mode="markers",
                hovertemplate=(
                    "Tick: %{x}<br>Status: HIT<br>%{customdata}<extra></extra>"
                ),
            )
        )
        graph.add_trace(
            go.Scatter(
                x=misses["Tick"],
                y=misses["Addr"],
                name="MISS",
                legendgrouptitle_text="DCACHE",
                customdata=misses["CustomData"],
                marker_color="red",
                marker_symbol="diamond",
                mode="markers",
                hovertemplate=(
                    "Tick: %{x}<br>Status: MISS<br>%{customdata}<extra></extra>"
                ),
            )
        )

        if len(all_addr) == 0:
            return None
        hitrate = hits_len / (hits_len + misses_len) * 100

        graph.update_xaxes(title_text="Duration (ticks)")
        unique_addrs = list(set(all_addr))
        graph.update_yaxes(
            categoryorder="array",
            categoryarray=sorted(unique_addrs, key=lambda x: int(x, 16)),
            title_text="Address DDR",
        )
        graph.update_layout(
            title=dict(
                text="<br><b>Data Cache Hit/Miss Chart<b>",
                subtitle_text=f"<b>Core Type:</b> {CoreType(core_type_id).name}"
                f"<br><b>Hit Rate:</b> {hitrate:.2f}%",
                automargin=True,
            )
        )
        return graph

    def _process(self) -> None:
        if not database_handler.table_has_data("DCacheHitMissEvents"):
            return
        for core_id in BackendDBManager.get_active_cores():
            figures: List[go.Figure] = []

            core_type_ids = BackendDBManager.get_active_core_types_ordered()
            for core_type_id in core_type_ids:
                fig = self.generate_subreport(core_id, core_type_id)
                if fig is not None:
                    figures.append(fig)

            if not figures:
                continue

            report_file_name = f"qi_core_{core_id}_dcache_utilization_report.html"
            report_path = os.path.join(self.output_dir, report_file_name)
            self.save_multifigures_report(figures, report_path)

    def process(self) -> None:
        with StageReporter("[QI] Data cache utilization report generation"):
            self._process()
        final_logger.add_general_info(f"Data Cache reports: {self.output_dir}")
