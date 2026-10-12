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
import sqlite3
from typing import Set, List
from npu_emulate.src.backend.hints.base.hint_base import IHint
from npu_emulate.src.backend.database import database_handler
import re


class VectorFusionHint(IHint):
    p_sn = r"Sn\[\d+\]=(?P<sn>0x[0-9a-z]+), "
    p_sn2 = r"(?:Sn2\[\d+\]=(?P<sn2>0x[0-9a-z]+), )?"
    p_am = r"Am\[\d+\]=(?P<am>0x[0-9a-z]+), "
    pattern = re.compile(rf".*{p_sn}{p_sn2}{p_am}")

    def __init__(self) -> None:
        super().__init__()
        self.hint_pcs: Set[int] = set()

        vf_list = self.db_retrieve_vf()
        ld_st_list = self.db_retrieve_ld_st()
        self.find_hinted_pcs(vf_list, ld_st_list)

    def db_retrieve_ld_st(self) -> List[sqlite3.Row]:
        gm_instrs = ("RV_VLD", "RV_VST")
        list_str = str(gm_instrs).replace(",)", ")")

        query = database_handler.execute(
            """
        SELECT ExecInstrTickEnd as TickEnd,
               ExecInstrName as Name,
               ExecInstrParams as Params
        FROM ExecutedInstructions
        WHERE ExecInstrName IN {}
        ORDER BY ExecInstrCoreId, ExecInstrId
        """.format(list_str)
        )
        return query.fetchall()

    def db_retrieve_vf(self) -> List[sqlite3.Row]:
        query = database_handler.execute("""
        SELECT SourceInstrAddr as PC,
               ExecInstrTickEnd as TickEnd
        FROM ExecutedInstructions
        WHERE ExecInstrName == 'VF' OR ExecInstrName == 'VFI'
        ORDER BY ExecInstrCoreId, ExecInstrId
        """)
        return query.fetchall()

    def compare_sets(self, set1: Set[int], set2: Set[int]) -> bool:
        if len(set1) == 0 or len(set2) == 0:
            return False
        breakpoint = min(len(set1), len(set2)) * 0.7
        return len(set1.intersection(set2)) > breakpoint

    def find_hinted_pcs(
        self, vf_list: List[sqlite3.Row], ld_st_list: List[sqlite3.Row]
    ) -> None:
        if not vf_list:
            return
        vf_index: int = 0
        vf_past_pc: int = -1
        st_past_set: Set[int] = set()
        st_set: Set[int] = set()
        ld_set: Set[int] = set()

        for instr in ld_st_list:
            while (
                vf_index < len(vf_list)
                and instr["TickEnd"] > vf_list[vf_index]["TickEnd"]
            ):
                if (
                    self.compare_sets(st_past_set, ld_set)
                    and vf_past_pc != vf_list[vf_index]["PC"]
                ):
                    self.hint_pcs.add(vf_list[vf_index]["PC"])
                st_past_set = st_set
                st_set = set()
                ld_set = set()
                vf_past_pc = vf_list[vf_index]["PC"]
                vf_index += 1
            params_raw = instr["Params"]
            if params_raw is None:
                continue
            mtch = self.pattern.match(params_raw)
            if mtch is None:
                log.debug(
                    f"VectorFusionHint: skip unparseable Params: {instr['Params']!r}"
                )
                continue
            addr: int = int(mtch["sn"], 16)
            if mtch["sn2"] is not None:
                addr += int(mtch["sn2"], 16) << 32
            addr += int(mtch["am"], 16)
            if instr["Name"] == "RV_VLD":
                ld_set.add(addr)
            else:
                st_set.add(addr)
        if vf_index < len(vf_list) and (
            self.compare_sets(st_past_set, ld_set)
            and vf_past_pc != vf_list[vf_index]["PC"]
        ):
            self.hint_pcs.add(vf_list[vf_index]["PC"])

    @staticmethod
    def get_name() -> str:
        return "vector_fusion_hint"

    def instruction_condition(self, instr: sqlite3.Row) -> bool:
        return instr["PC"] in self.hint_pcs

    def get_instruction_hint_text(self, instr: sqlite3.Row) -> str:
        return (
            "Fusion of sequential vector instructions failed "
            "because of non-vector instructions in-between. To "
            "turn on vector fusion, avoid putting non-vector "
            "instructions between vector ones."
        )
