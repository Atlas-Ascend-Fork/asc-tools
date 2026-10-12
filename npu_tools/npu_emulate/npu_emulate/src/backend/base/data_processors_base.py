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

import abc
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.biprof_context import biprof_ctx


class IBaseDataProcessor:
    def __init__(self) -> None:
        self._output_dir = biprof_ctx.output_dir
        self._analysis_name = biprof_ctx.analysis_type
        self._arch = biprof_ctx.arch

    @abc.abstractmethod
    def init(self) -> int:
        final_logger.add_general_info(f"Output folder: {self._output_dir}")
        return 0

    @abc.abstractmethod
    def process(self) -> int:  # Data processing AFTER the kernel's run
        pass


class IBaseReporter(IBaseDataProcessor):
    pass
