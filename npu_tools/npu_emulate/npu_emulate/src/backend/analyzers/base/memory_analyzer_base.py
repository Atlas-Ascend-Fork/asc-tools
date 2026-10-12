# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software: you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

import sqlite3

from npu_emulate.src.backend.analyzers.base.analyzer_base import IAnalyzer
from npu_emulate.src.common.extend_params import ExtendParams

ldg_type_map = {0: "u8", 1: "s8", 2: "u16", 3: "s16", 4: "b32", 5: "b64", 6: "b128"}

stg_type_map = {0: "b8", 1: "b16", 2: "b32", 3: "b64", 4: "b128"}


class MemoryAnalyzerBase(IAnalyzer):
    def __init__(self) -> None:
        super().__init__()

    @classmethod
    def match_type(cls, instr: sqlite3.Row) -> str:
        name = instr["ExecInstrName"]
        assert name in ["SIMT_STG", "SIMT_LDG", "SIMT_STS", "SIMT_LDS"]
        ep = ExtendParams.parse(instr["ExtendParams"])
        access_btype = ep.access_btype
        if access_btype is not None:
            return access_btype.lower()
        raise RuntimeError(
            f"Cannot extract access_btype from ExtendParams for instruction {name}"
        )
