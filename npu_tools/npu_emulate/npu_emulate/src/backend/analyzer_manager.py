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

import logging as log
from typing import Sequence, Type

from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer
from npu_emulate.src.backend.base.analyzer_manager_base import AnalyzerManagerBase
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.backend.analyzers.region_tracing_analyzer import (
    RegionTracingAnalyzer,
)
from npu_emulate.src.backend.analyzers.vector_utilization_analyzer import (
    VectorUtilizationAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.vf_simt_analyzer import (
    Ascend950VfSimtAnalyzer,
)
from npu_emulate.src.backend.analyzers.instr_units_usage_analyzer import (
    InstrUnitsUsageAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.live_registers_analyzer import (
    Ascend950LiveRegistersAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.basic_block_analyzer import (
    Ascend950BasicBlockAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.mask_analyzer import Ascend950MaskAnalyzer
from npu_emulate.src.backend.analyzers.david.simt_memory_access_analyzer import (
    Ascend950SIMTMemoryAccessAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.shared_bank_conflicts_analyzer import (
    Ascend950SharedBankConflictsAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.branch_efficiency_analyzer import (
    Ascend950BranchEfficiencyAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.warp_occupancy_analyzer import (
    Ascend950WarpOccupancyAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.dma_mov_align_analyzer import (
    Ascend950DMAMovAlignAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.dma_mov_multi_analyzer import (
    Ascend950DMAMovMultiAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.dma_mov_load_2d_analyzer import (
    Ascend950DMAMovLoad2DAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.dma_mov_load_3d_analyzer import (
    Ascend950DMAMovLoad3DAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.dma_mov_ub_analyzer import (
    Ascend950DMAMovUbAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.dma_mov_nd_dma_analyzer import (
    Ascend950DMAMovNdDmaAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.mmad_analyzer import Ascend950MmadAnalyzer
from npu_emulate.src.backend.analyzers.david.fixp_analyzer import Ascend950FixpAnalyzer
from npu_emulate.src.backend.analyzers.david.pipe_utilization_analyzer import (
    Ascend950PipeUtilizationAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.pipe_dependency_flags_analyzer import (
    Ascend950PipeDependencyFlagsAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.pipe_dependency_blocks_analyzer import (
    Ascend950PipeDependencyBlocksAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.pipe_dependency_buffers_analyzer import (
    Ascend950PipeDependencyBuffersAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.pipe_dependency_ffts_analyzer import (
    Ascend950PipeDependencyFFTSAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.simt_ld_st_analyzer import (
    Ascend950SIMTLdStAnalyzer,
)
from npu_emulate.src.backend.analyzers.arch_diagram_analyzer import ArchDiagramAnalyzer
from npu_emulate.src.backend.analyzers.david.simt_region_tracing_analyzer import (
    Ascend950SIMTRegionTracingAnalyzer,
)
from npu_emulate.src.backend.analyzers.call_stack_analyzer import CallStackAnalyzer
from npu_emulate.src.backend.analyzers.david.simd_region_tracing_analyzer import (
    Ascend950SIMDRegionTracingAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.memory_utilization_analyzer import (
    Ascend950MemoryUtilizationAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.vf_ipc_analyzer import (
    Ascend950VFIPCAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.vf_simt_ipc_analyzer import (
    Ascend950VFSimtIPCAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.scalar_ipc_analyzer import (
    Ascend950ScalarInstructionPerCycleAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.scalar_ipc_dynamic_analyzer import (
    Ascend950ScalarInstructionPerCycleDynamicAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.critical_path_analyzer import (
    CriticalPathAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.unit_utilization_analyzer import (
    Ascend950UnitUtilizationAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.fix_start_time_analyzer import (
    Ascend950FixStartTimeAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.split_compute_sync_queues_analyzer import (
    Ascend950SplitComputeSyncQueuesAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.simd_sampling_analyzer import (
    Ascend950SIMDSamplingAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.vf_memory_access_analyzer import (
    Ascend950VFMemoryAccessAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.subqueue_utilization_analyzer import (
    Ascend950SubqueueUtilizationAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.spr_instr_info_analyzer import (
    SPRInstrInfoAnalyzer,
)
from npu_emulate.src.backend.analyzers.vf_pmu.vf_pmu_analyzer import (
    VfPMUAnalyzer,
)
from npu_emulate.src.backend.analyzers.david.trace_bubble_analyzer import (
    Ascend950TraceBubbleAnalyzer,
)
from npu_emulate.src.common.biprof_enum import ArchName


class AnalyzerManager(AnalyzerManagerBase):
    # Static methods instead of simple class-level sequences
    # allow to use conditional elements
    @staticmethod
    def david_analyses() -> Sequence[Type[IAnalyzer]]:
        _analyzers = (
            Ascend950FixStartTimeAnalyzer,
            InstrUnitsUsageAnalyzer,
            VectorUtilizationAnalyzer,
            SPRInstrInfoAnalyzer,
            *([Ascend950VfSimtAnalyzer] if biprof_ctx.object_file else []),
            *([Ascend950LiveRegistersAnalyzer] if biprof_ctx.object_file else []),
            *([Ascend950BasicBlockAnalyzer] if biprof_ctx.object_file else []),
            Ascend950MaskAnalyzer,
            Ascend950SIMTMemoryAccessAnalyzer,
            Ascend950SharedBankConflictsAnalyzer,
            Ascend950BranchEfficiencyAnalyzer,
            *([Ascend950WarpOccupancyAnalyzer] if biprof_ctx.object_file else []),
            Ascend950DMAMovAlignAnalyzer,
            Ascend950DMAMovMultiAnalyzer,
            Ascend950DMAMovLoad2DAnalyzer,
            Ascend950DMAMovLoad3DAnalyzer,
            Ascend950DMAMovUbAnalyzer,
            Ascend950DMAMovNdDmaAnalyzer,
            Ascend950MmadAnalyzer,
            Ascend950FixpAnalyzer,
            Ascend950PipeUtilizationAnalyzer,
            Ascend950PipeDependencyFlagsAnalyzer,
            Ascend950PipeDependencyBlocksAnalyzer,
            Ascend950PipeDependencyBuffersAnalyzer,
            Ascend950PipeDependencyFFTSAnalyzer,
            Ascend950SIMTLdStAnalyzer,
            ArchDiagramAnalyzer,
            RegionTracingAnalyzer,
            *([Ascend950SIMTRegionTracingAnalyzer] if biprof_ctx.object_file else []),
            Ascend950SIMDRegionTracingAnalyzer,
            *(
                [Ascend950VFIPCAnalyzer, Ascend950VFSimtIPCAnalyzer]
                if biprof_ctx.vec_ipc
                else []
            ),
            Ascend950ScalarInstructionPerCycleAnalyzer,
            *(
                [Ascend950ScalarInstructionPerCycleDynamicAnalyzer]
                if biprof_ctx.scalar_ipc
                else []
            ),
            Ascend950UnitUtilizationAnalyzer,
            Ascend950SubqueueUtilizationAnalyzer,
            *([Ascend950TraceBubbleAnalyzer] if biprof_ctx.bubble else []),
            *([Ascend950SIMDSamplingAnalyzer] if biprof_ctx.simd_sampling else []),
            *([VfPMUAnalyzer] if biprof_ctx.simd_perf else []),
        )
        return _analyzers

    @staticmethod
    def david_smart_pipes_analyzers() -> Sequence[Type[IAnalyzer]]:
        return (Ascend950SplitComputeSyncQueuesAnalyzer,)

    @staticmethod
    def david_memory_utilization_analyser() -> Sequence[Type[IAnalyzer]]:
        return (Ascend950MemoryUtilizationAnalyzer,)

    @staticmethod
    def david_critical_path_analyzer() -> Sequence[Type[IAnalyzer]]:
        return (CriticalPathAnalyzer,)

    @staticmethod
    def david_call_stack_analyzer() -> Sequence[Type[IAnalyzer]]:
        return (CallStackAnalyzer,)

    @staticmethod
    def david_matrix_copy_analyzers() -> Sequence[Type[IAnalyzer]]:
        return (Ascend950VFMemoryAccessAnalyzer,)

    def __init__(self) -> None:
        super().__init__()
        arch = database_handler.get_arch()
        if arch == ArchName.V100:
            self.supported_analyses = list(
                self.david_analyses(),
            )
            if biprof_ctx.smart_pipes:
                self.supported_analyses = [
                    *self.david_smart_pipes_analyzers(),
                    *self.supported_analyses,
                ]
            if biprof_ctx.memory_utilization:
                self.supported_analyses = [
                    *self.supported_analyses,
                    *self.david_memory_utilization_analyser(),
                ]
            if biprof_ctx.critical_path:
                self.supported_analyses = [
                    *self.supported_analyses,
                    *self.david_critical_path_analyzer(),
                ]
            if biprof_ctx.call_stack:
                self.supported_analyses = [
                    *self.supported_analyses,
                    *self.david_call_stack_analyzer(),
                ]
            if biprof_ctx.matrix_copy:
                self.supported_analyses = [
                    *self.supported_analyses,
                    *self.david_matrix_copy_analyzers(),
                ]
        else:
            log.error("Unsupported simulator format.")
