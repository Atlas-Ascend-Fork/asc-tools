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

from dataclasses import dataclass
import sqlite3
from typing import Dict, List, Optional
from abc import ABC


@dataclass
class HintTarget:
    sourceline_id: Optional[int]
    pc: Optional[int]
    callstack_id: Optional[int]
    is_kernel: bool = False


@dataclass
class HintDataItem:
    hint_type_id: int
    hint_text: str
    targets: List[HintTarget]


class IHint(ABC):
    _counter: int = 1

    def __init__(self) -> None:
        self.id: int = self._counter
        self.result: List[HintDataItem] = []
        IHint._counter += 1

    def get_id(self) -> int:
        return self.id

    @staticmethod
    def get_name() -> str:
        raise NotImplementedError

    def instruction_condition(self, instr: sqlite3.Row) -> bool:
        return False

    def sourceline_condition(self, sourceline_instrs: List[sqlite3.Row]) -> bool:
        return False

    @classmethod
    def get_instruction_hint_text(cls, instr: sqlite3.Row) -> Optional[str]:
        return None

    @classmethod
    def get_sourceline_hint_text(
        cls, sourceline_instrs: List[sqlite3.Row]
    ) -> Optional[str]:
        return None

    def add_hint(self, hint_text: str, targets: List[HintTarget]) -> None:
        self.result.append(HintDataItem(self.get_id(), hint_text, targets))

    def process(
        self, instrs_map: Dict[Optional[int], List[sqlite3.Row]]
    ) -> List[HintDataItem]:
        for line_id, line_instrs in instrs_map.items():
            line_instr_hinted = False
            # Checking instruction_condition for every instruction
            for instr in line_instrs:
                instr_pc = int(instr["PC"])
                if self.instruction_condition(instr):
                    instr_hint_text = self.get_instruction_hint_text(instr)
                    assert instr_hint_text, "get_instruction_hint_text not implemented"
                    self.add_hint(
                        instr_hint_text,
                        [HintTarget(line_id, instr_pc, instr["CallStackId"])],
                    )
                    line_instr_hinted = True
            # Checking sourceline_condition for sourceline
            # if no instructions on that sourceline hinted
            # (by that type of hint)
            if line_id is not None and not line_instr_hinted:
                if self.sourceline_condition(line_instrs):
                    sourceline_hint_text = self.get_sourceline_hint_text(line_instrs)
                    assert sourceline_hint_text, (
                        "get_sourceline_hint_text not implemented"
                    )
                    self.add_hint(
                        sourceline_hint_text,
                        [HintTarget(line_id, None, line_instrs[0]["CallStackId"])],
                    )
        return self.result
