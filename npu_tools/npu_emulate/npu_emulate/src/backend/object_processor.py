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

from typing import List, Dict, Tuple

from npu_emulate.src.common.biprof_enum import ArchName
from npu_emulate.src.backend.elf_object_reader import (
    ElfObjectReader,
    FunctionInfo,
    ProcessedObjData,
)


class ObjectProcessor:
    def __init__(self, arch: ArchName, workspace: str, obj_file_name: str) -> None:
        self._arch = arch
        self._workspace = workspace
        self._obj_file_name = obj_file_name
        self.object_reader = ElfObjectReader(arch, workspace)

    def read_object_functions(self) -> Tuple[List[FunctionInfo], Dict[str, int]]:
        functions = self.object_reader.read_functions_info(self._obj_file_name)
        hidumper_funcs = self.object_reader.read_hidumper_detected_functions_info(
            self._obj_file_name
        )
        functions.extend(hidumper_funcs)
        # Stores kernel_name -> addr
        stripped_kernel_names: Dict[str, int] = {}

        for func in functions:
            if func.is_kernel:
                name, addr = func.name, func.addr
                stripped_name = name
                if name.endswith("_mix_aic") or name.endswith("_mix_aiv"):
                    stripped_name = name[: -len("_mix_ai*")]
                if stripped_name not in stripped_kernel_names:
                    stripped_kernel_names[stripped_name] = addr
                else:
                    stripped_kernel_names[stripped_name] = min(
                        addr, stripped_kernel_names[stripped_name]
                    )
        return functions, stripped_kernel_names

    def read(self, kernel_name: str) -> ProcessedObjData:
        return self.object_reader.read(self._obj_file_name, kernel_name)
