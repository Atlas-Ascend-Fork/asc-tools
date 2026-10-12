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
import re
from typing import List

from npu_emulate.src.backend.database_manager import BackendDBManager


class IAnalyzer(abc.ABC):
    def check_requirements(self) -> None:
        return

    def process(self) -> None:
        raise NotImplementedError

    def cleanup(self) -> None:
        pass

    @staticmethod
    def get_name() -> str:
        raise NotImplementedError

    @staticmethod
    def get_dtype_size(dtype: str) -> int:
        dtype_map = {
            "b8": 1,
            "s8": 1,
            "u8": 1,
            "b16": 2,
            "f16": 2,
            "s16": 2,
            "u16": 2,
            "bf16": 2,
            "fmix": 2,
            "b32": 4,
            "b32s": 4,
            "s32": 4,
            "f32": 4,
            "u32": 4,
            "s162s8": 2,
            "f162s8": 2,
            "f322f16": 4,
            "VDEQs162b8": 2,
            "f162u8": 2,
            "b64": 8,
            "b128": 16,
        }
        return dtype_map[dtype.lower()]

    @staticmethod
    def extract_str_value(pattern: str, string: str) -> str:
        val = re.compile(pattern).search(string)
        if val is None:
            raise RuntimeError(f"Unable to find pattern {pattern} in a string {string}")
        return val[1]

    @staticmethod
    def extract_int_value(pattern: str, string: str, base: int = 10) -> int:
        return int(IAnalyzer.extract_str_value(pattern, string), base=base)

    @staticmethod
    def get_active_cores() -> List[int]:
        return BackendDBManager.get_active_cores()

    @staticmethod
    def get_active_core_types() -> List[int]:
        return BackendDBManager.get_active_core_types()
