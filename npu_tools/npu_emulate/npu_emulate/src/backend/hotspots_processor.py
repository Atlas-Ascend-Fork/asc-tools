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
from typing import Optional

from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.frontend.tracing.chrome_tracing_default import ChromeTracingBuilder
from npu_emulate.src.frontend.tracing.chrome_tracing_smart_pipe import (
    SmartPipeTracingBuilder,
)
from npu_emulate.src.frontend.tracing.chrome_tracing_aicore_occupancy import (
    AiCoreOccupancyTraceBuilder,
)
from npu_emulate.src.frontend.tracing.chrome_tracing_critical_path import (
    CriticalPathBuilder,
)
from npu_emulate.src.frontend.source_asm_html import SourceAsmHTMLGenerator
from npu_emulate.src.arch_diagram.reporter import ArchDiagramReporter
from npu_emulate.src.frontend.mcv_html import MatrixCopyingVizualizerHTMLGenerator
from npu_emulate.src.arch_diagram.constants import ARCH_DIAG_SUPPORTED_ARCH
from npu_emulate.src.frontend.plotly.issue_queue_utilization_reporter import (
    IssueQueueUtilizationReporter,
)
from npu_emulate.src.frontend.plotly.scalar_ipc_dynamic_reporter import (
    ScalarIPCDynamicReporter,
)
from npu_emulate.src.frontend.plotly.vec_ipc_reporter import VecIPCReporter
from npu_emulate.src.frontend.plotly.vec_ipc_dynamic_reporter import (
    VecIPCDynamicReporter,
)
from npu_emulate.src.frontend.plotly.instr_duration_distribution_html import (
    InstrDistributionHTMLGenerator,
)
from npu_emulate.src.frontend.plotly.data_cache_utilization_reporter import (
    DataCacheUtilizationReporter,
)
from npu_emulate.src.frontend.roofline_reporter import RooflineReporter
from npu_emulate.src.frontend.plotly.memory_latencies_reporter import (
    MemoryLatenciesReporter,
)
from npu_emulate.src.frontend.plotly.scalar_pmu_performance_reporter import (
    ScalarPMUPerformanceReporter,
)
from npu_emulate.src.frontend.plotly.vf_pmu_topdown_reporter import (
    VfPMUTopDownReporter,
)
from npu_emulate.src.frontend.plotly.vf_pmu_counter_chart import (
    VfPMUCounterChart,
)
from npu_emulate.src.frontend.plotly.vf_pmu_perf_summary_reporter import (
    VfPMUPerfSummaryReporter,
)
from npu_emulate.src.frontend.plotly.vf_pmu_perf_per_vf_reporter import (
    VfPMUPerfPerVfReporter,
)
from npu_emulate.src.frontend.summary_reporter import SummaryReporter
from npu_emulate.src.frontend.bubble_report import BubbleReporter
from npu_emulate.src.common.biprof_enum import ArchName


class HotspotsProcessor:
    def __init__(
        self,
        arch: str,
        workspace: str,
        log_folder: str,
        log_files: list,
        object_path: Optional[str] = None,
        kernel_name: str = "kernel",
        kernel_name_short: str = "kernel",
        kernel_addr: int = 0x0,
        report_dir_name: Optional[str] = None,
    ) -> None:
        self.arch = arch
        self.log_folder = log_folder
        self.object_path = object_path
        self.workspace = workspace
        self.kernel_name = kernel_name
        self.kernel_name_short = kernel_name_short
        self.kernel_addr = kernel_addr
        self.report_dir_name = report_dir_name or f"{kernel_name_short}_reports"
        self.report_dir_path = os.path.join(workspace, self.report_dir_name)
        os.mkdir(self.report_dir_path)

    def generate_source_assembly(self) -> int:
        sareporter = SourceAsmHTMLGenerator()
        return sareporter.prepare_report(self.report_dir_path, self.kernel_name)

    def generate_chrome_tracing(self) -> int:
        builder = ChromeTracingBuilder()
        occupancy_builder = AiCoreOccupancyTraceBuilder()
        res = builder.generate(self.report_dir_path)
        if biprof_ctx.smart_pipes:
            smart_pipe_builder = SmartPipeTracingBuilder()
            res += smart_pipe_builder.generate(self.report_dir_path)
        res += occupancy_builder.generate(self.report_dir_path)
        if biprof_ctx.critical_path:
            cpath_builder = CriticalPathBuilder()
            res += cpath_builder.generate(self.report_dir_path)
        return res

    def generate_arch_diagram(self) -> int:
        if self.arch in ARCH_DIAG_SUPPORTED_ARCH:
            adreporter = ArchDiagramReporter(self.report_dir_path)
            return adreporter.process()
        return 0

    def generate_issue_queue_utilization_report(self) -> int:
        reporter = IssueQueueUtilizationReporter(self.report_dir_path)
        reporter.process()
        return 0

    def generate_mcv_html(self) -> int:
        reporter = MatrixCopyingVizualizerHTMLGenerator()
        return reporter.prepare_report(self.report_dir_path)

    def generate_scalar_ipc_dynamic_report(self) -> int:
        reporter = ScalarIPCDynamicReporter(self.report_dir_path)
        reporter.process()
        return 0

    def generate_vf_ipc_reports(self) -> int:
        reporter = VecIPCReporter(self.report_dir_path)
        reporter.process()

        reporter_dynamic = VecIPCDynamicReporter(self.report_dir_path)
        reporter_dynamic.process()
        return 0

    def generate_instr_duration_distribution_report(self) -> int:
        instr_dur_distr = InstrDistributionHTMLGenerator(self.report_dir_path)
        instr_dur_distr.process()
        return 0

    def generate_icache_report(self) -> int:
        return 0

    def generate_data_cache_report(self) -> int:
        if not database_handler.table_has_data("DCacheHitMissEvents"):
            return 0
        reporter = DataCacheUtilizationReporter(self.report_dir_path)
        reporter.process()
        return 0

    def generate_roofline_report(self) -> int:
        roofline_reporter = RooflineReporter()
        return roofline_reporter.prepare_report(self.report_dir_path)

    def generate_memory_latencies_reports(self) -> int:
        memory_latencies_reporter = MemoryLatenciesReporter(self.report_dir_path)
        memory_latencies_reporter.process()
        return 0

    def generate_scalar_pmu_performance_report(self) -> int:
        reporter = ScalarPMUPerformanceReporter(self.report_dir_path)
        reporter.process()
        return 0

    def generate_simd_performance_reports(self) -> int:
        if biprof_ctx.arch != ArchName.V100:
            return 0
        simd_perf_path = os.path.join(self.report_dir_path, "simd-performance")
        os.makedirs(simd_perf_path, exist_ok=True)
        VfPMUTopDownReporter(simd_perf_path).process()
        VfPMUCounterChart(simd_perf_path).process()
        VfPMUPerfSummaryReporter(simd_perf_path).process()
        VfPMUPerfPerVfReporter(simd_perf_path).process()
        if not os.listdir(simd_perf_path):
            os.rmdir(simd_perf_path)
        return 0

    def generate_bubble_report(self) -> int:
        reporter = BubbleReporter()
        reporter.prepare_report(self.report_dir_path)
        return 0

    def generate_summary_report(self) -> int:
        summary_reporter = SummaryReporter()
        return summary_reporter.prepare_report(self.report_dir_path)
