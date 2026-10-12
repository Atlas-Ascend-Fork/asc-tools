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
from typing import Any, List, Tuple
from npu_emulate.src.common.biprof_enum import CoreType, ICacheEventType
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.frontend.base.plotly_reporter_base import PlotlyReporterBase


class ICacheUtilizationReporter(PlotlyReporterBase):
    # Single shared hovertemplate. %{customdata[i]} is read from a 2-column
    # array [PC, Size]. event_type.name is baked in via .format up front --
    # one short string per trace instead of a million f-strings per point.
    HOVER_TPL = (
        "Tick: %{{x}}<br>"
        "Status: {status}<br>"
        "PC: %{{customdata[0]:#x}}<br>"
        "Size: %{{customdata[1]:#x}}<br>"
        "Instruction Cache Type: {ev_name}<extra></extra>"
    )

    @staticmethod
    def get_icache_events(
        core_id: int, core_type_id: int, hs: int, event_type: ICacheEventType
    ) -> Tuple[List[int], List[int], List[List[int]]]:
        cur = database_handler.execute(
            """
        SELECT  EventTimeTick AS Tick,
                SourceInstrAddr AS PC,
                EventSize
        FROM ICacheEvents
        WHERE CoreId=:core_id AND
              CoreTypeID=:core_type_id AND
              HitStatus=:hit_status AND
              ICacheEventType=:event_type
        ORDER BY SourceInstrAddr
        """,
            {
                "core_id": core_id,
                "core_type_id": core_type_id,
                "hit_status": hs,
                "event_type": event_type.value,
            },
        )

        ticks: List[int] = []
        pcs: List[int] = []
        customdata: List[List[int]] = []
        # Stream rows from the cursor instead of fetchall() so the full
        # result set does not coexist in memory with the three lists.
        for r in cur:
            ticks.append(r["Tick"])
            pcs.append(r["PC"])
            customdata.append([r["PC"], r["EventSize"]])
        return ticks, pcs, customdata

    @staticmethod
    def get_pc_stats(core_id: int, core_type_id: int) -> Tuple[int, int, int]:
        """Compute min/max PC and total event count in SQL, not in Python."""
        row = database_handler.execute(
            """
        SELECT MIN(SourceInstrAddr) AS MinPC,
               MAX(SourceInstrAddr) AS MaxPC,
               COUNT(*)              AS Cnt
        FROM ICacheEvents
        WHERE CoreId=:core_id AND CoreTypeID=:core_type_id
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchone()
        return row["MinPC"], row["MaxPC"], row["Cnt"]

    @staticmethod
    def get_hit_count(core_id: int, core_type_id: int) -> Any:
        return database_handler.execute(
            """
        SELECT COUNT(*) AS Cnt FROM ICacheEvents
        WHERE CoreId=:core_id AND CoreTypeID=:core_type_id AND HitStatus=1
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchone()["Cnt"]

    def get_vector_border(self, core_id: int, core_type_id: int) -> Any:
        border = database_handler.execute(
            """
        SELECT SourceInstrAddr FROM ExecutedInstructions
        WHERE (InstrTypeId < 50 OR InstrTypeId == 55)
              AND ExecInstrCoreId=:core_id
              AND CoreTypeId=:core_type_id
        ORDER BY SourceInstrAddr DESC
        LIMIT 1
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        )
        return border.fetchone()["SourceInstrAddr"]

    def check_vector_existence(self, core_id: int, core_type_id: int) -> Any:
        # EXISTS is cheaper than COUNT -- the DB stops at the first match.
        row = database_handler.execute(
            """
        SELECT EXISTS(
            SELECT 1 FROM ExecutedInstructions
            WHERE InstrTypeId >= 50 AND InstrTypeId < 55
                  AND ExecInstrCoreId=:core_id
                  AND CoreTypeId=:core_type_id
        ) AS HasVec
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchone()
        return bool(row["HasVec"])

    def get_start_pc(self, core_id: int, core_type_id: int) -> Any:
        start_pc = database_handler.execute(
            """
        SELECT StartingPC FROM ICacheStartingPCs
        WHERE CoreId=:core_id AND CoreTypeId=:core_type_id
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchone()["StartingPC"]
        return start_pc

    def _add_trace(
        self,
        graph: go.Figure,
        ticks: List[int],
        pcs: List[int],
        customdata: List[List[int]],
        *,
        status: str,
        color: str,
        symbol: str,
        event_type: ICacheEventType,
    ) -> None:
        graph.add_trace(
            go.Scatter(
                x=ticks,
                y=pcs,
                name=status,
                legendgroup=event_type.value,
                legendgrouptitle_text=event_type.name,
                customdata=customdata,
                marker_color=color,
                marker_symbol=symbol,
                mode="markers",
                hovertemplate=self.HOVER_TPL.format(
                    status=status, ev_name=event_type.name
                ),
            )
        )

    def generate_subreport(self, core_id: int, core_type_id: int) -> go.Figure:
        graph = go.Figure()
        symbol_types = ("circle", "diamond")

        min_pc, max_pc, total = self.get_pc_stats(core_id, core_type_id)
        if total == 0:
            graph.update_layout(title="Instruction Cache data not available")
            return graph

        for et in ICacheEventType:
            symbol = symbol_types[et.value - 1]
            for hs, status, color in ((1, "HIT", "blue"), (0, "MISS", "red")):
                ticks, pcs, custom = self.get_icache_events(
                    core_id, core_type_id, hs, et
                )
                if not ticks:
                    continue
                self._add_trace(
                    graph,
                    ticks,
                    pcs,
                    custom,
                    status=status,
                    color=color,
                    symbol=symbol,
                    event_type=et,
                )
                # Release immediately -- Plotly has already copied the data
                # into the trace, so the local lists can be freed before the
                # next iteration allocates a new batch.
                del ticks, pcs, custom

        hits_len = self.get_hit_count(core_id, core_type_id)
        hitrate = hits_len / total * 100
        start_pc = self.get_start_pc(core_id, core_type_id)

        step = (max_pc - min_pc) // 6
        if step == 0:
            tickvals = [min_pc]
        else:
            tickvals = list(range(min_pc, max_pc, step))
        graph.update_yaxes(
            tickvals=tickvals,
            ticktext=[hex(v) for v in tickvals],
        )

        border = self.get_vector_border(core_id, core_type_id)
        vector_exists = self.check_vector_existence(core_id, core_type_id)
        if border >= min_pc:
            graph.add_hrect(
                y0=min_pc,
                y1=border,
                line_width=0,
                fillcolor="red",
                opacity=0.1,
                annotation_text="Scalar Instructions",
            )
        if border < max_pc and vector_exists:
            graph.add_hrect(
                y0=border,
                y1=max_pc,
                line_width=0,
                fillcolor="blue",
                opacity=0.1,
                annotation_text="Vector Instructions",
            )

        graph.update_xaxes(title_text="Duration (ticks)")
        graph.update_yaxes(title_text="PC (relative)")
        graph.update_layout(
            title=dict(
                text="<br><b>Instruction Cache Hit/Miss Chart</b>",
                subtitle_text=f"<b>Core Type:</b> {CoreType(core_type_id).name}"
                f"<br><b>Start PC:</b> {hex(start_pc)}"
                f"<br><b>Hit Rate:</b> {hitrate:.2f}%",
                automargin=True,
            )
        )
        return graph

    def _process(self) -> None:
        for core_id in BackendDBManager.get_active_cores():
            core_type_ids = BackendDBManager.get_active_core_types_ordered()
            figures: List[go.Figure] = []
            for core_type_id in core_type_ids:
                figures.append(self.generate_subreport(core_id, core_type_id))

            if not figures:
                continue

            report_file_name = f"qi_core_{core_id}_icache_utilization_report.html"
            report_path = os.path.join(self.output_dir, report_file_name)
            self.save_multifigures_report(figures, report_path)
            figures.clear()

    def process(self) -> None:
        with StageReporter("[QI] Instruction cache utilization report generation"):
            self._process()
        final_logger.add_general_info(f"Instruction Cache reports: {self.output_dir}")
