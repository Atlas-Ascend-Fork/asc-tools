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
from enum import Enum, auto
from typing import Dict, List


class ArchName(str, Enum):
    V100 = "v100"

    def __str__(self) -> str:
        return str(self.value)


user_visible_archs = list(ArchName)


class InstrUnit(Enum):
    SCALAR = auto()
    VEC = auto()
    CUBE = auto()
    MTE = auto()
    FIXP = auto()
    SIMT = auto()
    ALL = 50
    NOT_AVAILABLE = auto()

    def __str__(self) -> str:
        return str(self.name)


class InstrQueueType(Enum):
    SCALAR = auto()
    CUBE = auto()
    FIXP = auto()
    MTE1 = auto()
    MTE2 = auto()
    MTE3 = auto()
    SIMT = auto()
    SIMD = auto()
    NONE = auto()


class MemoryType(Enum):
    AIV0_UB = auto()
    AIV1_UB = auto()
    AIC_L1 = auto()
    AIC_L0A = auto()
    AIC_L0B = auto()
    AIC_L0C = auto()
    OUT = auto()


class CTSectionType(Enum):
    PRE = auto()
    INSTR = auto()
    POST = auto()

    def __str__(self) -> str:
        return str(self.name)


def queue_type_to_instr_types() -> Dict[InstrQueueType, List["InstrType"]]:
    return {
        InstrQueueType.SIMD: [
            InstrType.RVECEX,
            InstrType.RVECLP,
            InstrType.RVECSU,
            InstrType.RVECST,
            InstrType.RVECLD,
        ],
        InstrQueueType.SCALAR: [InstrType.SCALAR, InstrType.FC, InstrType.PUSHQ],
        InstrQueueType.CUBE: [InstrType.CUBE],
        InstrQueueType.MTE1: [InstrType.MTE1],
        InstrQueueType.MTE2: [InstrType.MTE2],
        InstrQueueType.MTE3: [InstrType.MTE3],
        InstrQueueType.FIXP: [InstrType.FIXP],
        InstrQueueType.SIMT: [
            InstrType.RVECEX,
            InstrType.RVECLP,
            InstrType.RVECSU,
            InstrType.RVECST,
            InstrType.RVECLD,
        ],
    }


class BiprofMode(str, Enum):
    REPORT = "report"

    def __str__(self) -> str:
        return str(self.value)


class AnalysisType(str, Enum):
    HOTSPOTS = "hotspots"

    def __str__(self) -> str:
        return str(self.value)


class CoreType(Enum):
    AIC = auto()
    AIV0 = auto()
    AIV1 = auto()

    def __format__(self, format_spec: str) -> str:
        return f"{self.name:{format_spec}}"


class LogLevel:
    CRITICAL = "critical"
    DEBUG = "debug"
    ERROR = "error"
    INFO = "info"
    NOTSET = "notset"
    WARNING = "warning"


class ArgumentAction:
    STORE = "store"
    STORE_TRUE = "store_true"
    STORE_FALSE = "store_false"
    STORE_CONST = "store_const"
    APPEND = "append"
    APPEND_CONST = "append_const"
    COUNT = "count"
    HELP = "help"
    VERSION = "version"
    EXTEND = "extend"


class InstrType(Enum):
    SCALAR = auto()
    CUBE = auto()
    MTE1 = auto()
    MTE2 = auto()
    MTE3 = auto()
    FC = auto()
    FIXP = auto()
    RVECEX = 50
    RVECLP = auto()
    RVECSU = auto()
    RVECST = auto()
    RVECLD = auto()
    PUSHQ = auto()
    ALL = 99
    NOT_AVAILABLE = 100

    def __str__(self) -> str:
        return self.name

    def __format__(self, format_spec: str) -> str:
        return f"{self.name:{format_spec}}"


class UbAccessType(Enum):
    SIMT_RD = auto()
    SIMT_WR = auto()
    SIMD_RD = auto()
    SIMD_WR = auto()


class SubQueueType(Enum):
    EXEC = auto()
    FLAG = auto()
    PUSH_PB = auto()


class ICacheEventType(Enum):
    SCALAR = auto()
    VECTOR = auto()

    def __str__(self) -> str:
        return self.name


class BrifEventType(Enum):
    SEND_CMD_EVENT = auto()
    RECV_DATA_EVENT = auto()
    PUSH_DATA_EVENT = auto()

    def __str__(self) -> str:
        return self.name


@dataclass
class BrifEvent:
    core_id: int
    event_type: int
    event_tick: int
    mte_instr_id: int
    addr: str
    tag_id: int
    group_id: int
