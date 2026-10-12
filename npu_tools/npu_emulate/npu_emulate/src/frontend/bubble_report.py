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

import json
import logging as log
import traceback
from typing import Any, Dict, List

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_enum import CoreType
from npu_emulate.src.common.stage_reporter import StageReporter


class BubbleReporter:
    @staticmethod
    def _get_summaries() -> List[Any]:
        if not database_handler.table_exists("TraceBubbleSummary"):
            return []
        return database_handler.execute("""
        SELECT CoreId, CoreTypeId, MainPipeline, TotalDurationUs,
               BubbleCount, TopBottleneck, SubtypeSummary
        FROM TraceBubbleSummary
        ORDER BY CoreId, CoreTypeId
        """).fetchall()

    @staticmethod
    def _get_bubbles(core_id: int, core_type_id: int) -> List[Any]:
        return database_handler.execute(
            """
        SELECT SubType, Category, TickStart, TickEnd, GapTicks, GapUs, Reason,
               Optimizable, OptimizationHint, WaitedPipeline, IterationIndex
        FROM TraceBubbles
        WHERE CoreId = :core_id AND CoreTypeId = :core_type_id
        ORDER BY TickStart
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchall()

    @staticmethod
    def _get_top_5_longest_bubbles(core_id: int, core_type_id: int) -> List[Any]:
        return database_handler.execute(
            """
        SELECT SubType, Category, TickStart, TickEnd, GapTicks, GapUs, Reason,
               Optimizable, OptimizationHint, WaitedPipeline, IterationIndex
        FROM TraceBubbles
        WHERE CoreId = :core_id AND CoreTypeId = :core_type_id
        ORDER BY GapTicks DESC
        LIMIT 5
        """,
            {"core_id": core_id, "core_type_id": core_type_id},
        ).fetchall()

    @classmethod
    def _build_report_detailed(cls) -> List[Dict[str, Any]]:
        report: List[Dict[str, Any]] = []
        for summary in cls._get_summaries():
            bubbles = cls._get_bubbles(summary["CoreId"], summary["CoreTypeId"])
            report.append(
                {
                    "core_id": summary["CoreId"],
                    "core_type": CoreType(summary["CoreTypeId"]).name,
                    "main_pipeline": summary["MainPipeline"],
                    "total_duration_us": summary["TotalDurationUs"],
                    "bubble_count": summary["BubbleCount"],
                    "top_bottleneck": summary["TopBottleneck"],
                    "subtype_summary": summary["SubtypeSummary"],
                    "bubbles": [
                        {
                            "sub_type": b["SubType"],
                            "category": b["Category"],
                            "tick_start": b["TickStart"],
                            "tick_end": b["TickEnd"],
                            "gap_ticks": b["GapTicks"],
                            "gap_us": b["GapUs"],
                            "reason": b["Reason"],
                            "optimizable": bool(b["Optimizable"]),
                            "optimization_hint": b["OptimizationHint"],
                            "waited_pipeline": b["WaitedPipeline"],
                            "iteration_index": b["IterationIndex"],
                        }
                        for b in bubbles
                    ],
                }
            )
        return report

    @classmethod
    def _build_report_summary(cls) -> List[Dict[str, Any]]:
        report: List[Dict[str, Any]] = []
        for summary in cls._get_summaries():
            bubbles = cls._get_top_5_longest_bubbles(
                summary["CoreId"], summary["CoreTypeId"]
            )
            report.append(
                {
                    "core_id": summary["CoreId"],
                    "core_type": CoreType(summary["CoreTypeId"]).name,
                    "main_pipeline": summary["MainPipeline"],
                    "total_duration_us": summary["TotalDurationUs"],
                    "bubble_count": summary["BubbleCount"],
                    "top_bottleneck": summary["TopBottleneck"],
                    "subtype_summary": summary["SubtypeSummary"],
                    "top_5_longest_bubbles": [
                        {
                            "sub_type": b["SubType"],
                            "category": b["Category"],
                            "tick_start": b["TickStart"],
                            "tick_end": b["TickEnd"],
                            "gap_ticks": b["GapTicks"],
                            "gap_us": b["GapUs"],
                            "reason": b["Reason"],
                            "optimizable": bool(b["Optimizable"]),
                            "optimization_hint": b["OptimizationHint"],
                            "waited_pipeline": b["WaitedPipeline"],
                            "iteration_index": b["IterationIndex"],
                        }
                        for b in bubbles
                    ],
                }
            )
        return report

    def prepare_report(self, output_dir: str) -> int:
        try:
            with StageReporter("Bubble reporter: getting JSON data"):
                report = self._build_report_summary()
                if not report:
                    log.debug("No bubble analysis data, skipping report")
                    return 0
                with open(f"{output_dir}/bubble_report_summary.json", "w") as fp:
                    json.dump(report, fp, indent=2, ensure_ascii=False)
                report = self._build_report_detailed()
                if not report:
                    log.debug("No bubble analysis data, skipping report")
                    return 0
                with open(f"{output_dir}/bubble_report_detailed.json", "w") as fp:
                    json.dump(report, fp, indent=2, ensure_ascii=False)
            return 0
        except Exception as error:
            log.warning("Bubble reporter skipped")
            log.warning(str(error))
            log.debug(traceback.format_exc())
            return 0
