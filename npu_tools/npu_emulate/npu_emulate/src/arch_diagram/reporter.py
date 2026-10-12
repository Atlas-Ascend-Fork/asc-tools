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

from npu_emulate.src.backend.base.data_processors_base import IBaseReporter
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.common.biprof_enum import CoreType
from npu_emulate.src.arch_diagram.viewset import ArchDiagramViewSet


class ArchDiagramReporter(IBaseReporter):
    def __init__(self, output_path: str) -> None:
        super().__init__()
        self._input_dir: str = ""
        self._output_dir: str = output_path
        self._arch: str = biprof_ctx.arch
        self._veccore_cnt: int = self.obtain_veccore_cnt()

    def init(self) -> int:
        ret = super().init()
        return ret

    def process(self) -> int:
        processor = ArchDiagramProcessor(
            self._input_dir, self._output_dir, self._arch, self._veccore_cnt
        )
        return processor.process()

    def obtain_veccore_cnt(self) -> int:
        sql_query = "SELECT DISTINCT CoreTypeId from ExecutedInstructions"
        core_type_list = database_handler.execute(sql_query).fetchall()
        r = [i["CoreTypeId"] for i in core_type_list]
        if CoreType.AIV1.value in r:
            return 2
        return 1


class ArchDiagramProcessor:
    def __init__(
        self, input_dir: str, output_dir: str, arch: str, veccore_cnt: int
    ) -> None:
        self._input_dir: str = input_dir
        self._output_dir: str = output_dir
        self._arch: str = arch
        self._veccore_cnt = veccore_cnt

    def process(self) -> int:
        view_set = ArchDiagramViewSet(self._arch, self._veccore_cnt)
        ret = view_set.process_data(self._input_dir)
        ret += view_set.generate_diagrams(self._output_dir)
        return ret
