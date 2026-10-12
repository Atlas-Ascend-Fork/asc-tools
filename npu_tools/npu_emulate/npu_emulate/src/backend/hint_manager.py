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

from typing import Sequence, Type

from npu_emulate.src.backend.base.hint_manager_base import HintManagerBase
from npu_emulate.src.backend.hints.base.hint_base import IHint
from npu_emulate.src.backend.hints.shared_bank_conflicts_hint import (
    SharedBankConflictsHint,
)
from npu_emulate.src.backend.hints.mte_calls_hint import MTECallsHint
from npu_emulate.src.backend.hints.vector_utilization_hint import VectorUtilizationHint
from npu_emulate.src.backend.hints.mix_gm_hint import MixGMHint
from npu_emulate.src.backend.hints.access_pattern_hint import AccessPatternHint
from npu_emulate.src.backend.hints.small_grid_hint import SmallGridHint
from npu_emulate.src.backend.hints.tail_effect_hint import TailEffectHint
from npu_emulate.src.backend.hints.single_core_hint import SingleCoreHint
from npu_emulate.src.backend.hints.load_imbalance_hint import LoadImbalanceHint
from npu_emulate.src.backend.hints.aic_underutilization_hint import (
    AicUnderutilizationHint,
)
from npu_emulate.src.backend.hints.aiv_underutilization_hint import (
    AivUnderutilizationHint,
)
from npu_emulate.src.backend.hints.low_vec_mte2_overlap_hint import (
    LowVecMte2OverlapHint,
)
from npu_emulate.src.backend.hints.low_vec_ipc_hint import LowVecIpcHint


class HintManager(HintManagerBase):
    # Static methods instead of simple class-level sequences
    # allow to use conditional elements
    @staticmethod
    def david_hints() -> Sequence[Type[IHint]]:
        _hints = (
            SharedBankConflictsHint,
            MTECallsHint,
            VectorUtilizationHint,
            MixGMHint,
            AccessPatternHint,
            SmallGridHint,
            TailEffectHint,
            SingleCoreHint,
            LoadImbalanceHint,
            AicUnderutilizationHint,
            AivUnderutilizationHint,
            LowVecMte2OverlapHint,
            LowVecIpcHint,
        )
        return _hints

    def __init__(self) -> None:
        super().__init__()
        self.supported_hints = list(self.david_hints())
