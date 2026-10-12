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

from npu_emulate.src.arch_diagram.view import ViewData
from npu_emulate.src.arch_diagram.metric_group import (
    ArchDiagramMetricGroup as ADMetGroup,
)
from npu_emulate.src.arch_diagram.groups import MetricGroups
from enum import Enum


class Views(Enum):
    AICORE_MEMORY_SIM_GBS_2_VEC_CORES = ViewData(
        "aicore_memory_sim",
        "gbs",
        (
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_CUBE_GBS.value),
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_VEC0_GBS.value),
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_VEC1_GBS.value),
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_HBM_GBS.value),
            ADMetGroup(MetricGroups.SIM_PIPE_UTIL_CUBE.value),
            ADMetGroup(MetricGroups.SIM_PIPE_UTIL_VEC0.value),
            ADMetGroup(MetricGroups.SIM_PIPE_UTIL_VEC1.value),
            ADMetGroup(MetricGroups.SIM_L2_CACHE_USAGE.value),
        ),
    )
    AICORE_MEMORY_SIM_CNT_2_VEC_CORES = ViewData(
        "aicore_memory_sim",
        "cnt",
        (
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_CUBE_CNT.value),
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_VEC0_CNT.value),
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_VEC1_CNT.value),
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_HBM_CNT.value),
            ADMetGroup(MetricGroups.SIM_PIPE_UTIL_CUBE.value),
            ADMetGroup(MetricGroups.SIM_PIPE_UTIL_VEC0.value),
            ADMetGroup(MetricGroups.SIM_PIPE_UTIL_VEC1.value),
            ADMetGroup(MetricGroups.SIM_L2_CACHE_USAGE.value),
        ),
    )
    AICORE_MEMORY_SIM_RATIO_2_VEC_CORES = ViewData(
        "aicore_memory_sim",
        "ratio",
        (
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_CUBE_RATIO.value),
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_VEC0_RATIO.value),
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_VEC1_RATIO.value),
            ADMetGroup(MetricGroups.SIM_MEMORY_TPUT_HBM_RATIO.value),
            ADMetGroup(MetricGroups.SIM_PIPE_UTIL_CUBE.value),
            ADMetGroup(MetricGroups.SIM_PIPE_UTIL_VEC0.value),
            ADMetGroup(MetricGroups.SIM_PIPE_UTIL_VEC1.value),
            ADMetGroup(MetricGroups.SIM_L2_CACHE_USAGE.value),
        ),
    )
