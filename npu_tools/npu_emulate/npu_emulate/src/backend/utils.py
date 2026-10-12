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

from numbers import Rational
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Optional
from npu_emulate.src.backend.database import database_handler


@dataclass
class StackRecord:
    stack_level: int
    file_path: str
    file_line: int
    caller_function_name: str


class CallStackUtils:
    @staticmethod
    def get_call_stack_map() -> Dict[int, List[StackRecord]]:
        stacks = database_handler.execute("""
            SELECT CallStacks.StackId AS StackId,
                   CallStacks.StackLevel AS StackLevel,
                   CallStacks.CallSiteLine AS Line,
                   CallFunctions.FunctionName AS FunctionName,
                   SourceFiles.SourceFilePath AS SourceFilePath
            FROM CallStacks
            LEFT JOIN CallFunctions
            ON CallFunctions.CallFunctionId=CallStacks.CallFunctionId
            LEFT JOIN SourceFiles
            ON CallFunctions.SourceFileId=SourceFiles.SourceFileId
            """).fetchall()
        stack_map = defaultdict(list)
        for st in stacks:
            stack_map[st["StackId"]].append(
                StackRecord(
                    st["StackLevel"],
                    st["SourceFilePath"],
                    st["Line"],
                    st["FunctionName"],
                )
            )
        return stack_map

    @staticmethod
    def get_instruction_call_stack(
        call_stack_map: Dict[int, List[StackRecord]], stack_id: int
    ) -> List[str]:
        instr_stacks = call_stack_map[stack_id]
        instr_stacks.sort(key=lambda x: x.stack_level)
        res = []
        for st in instr_stacks:
            res.append(f"{st.file_path}:{st.file_line}\n({st.caller_function_name})\n")
        return res


class ChromeTracingUtils:
    @classmethod
    def percents_or_none(cls, value: Optional[Rational]) -> Optional[str]:
        if value is None:
            return None
        return f"{value}%"

    @classmethod
    def hex_or_none(cls, value: Optional[int]) -> Optional[str]:
        if value is None:
            return None
        return hex(value)

    @classmethod
    def round_or_none(cls, value: Optional[Rational], ndigits: int) -> Optional[float]:
        if value is None:
            return None
        return float(round(value, ndigits))
