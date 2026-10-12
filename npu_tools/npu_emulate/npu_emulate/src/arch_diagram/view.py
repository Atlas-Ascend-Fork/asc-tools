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
import os
import logging as log
from typing import Dict
from collections import defaultdict
from npu_emulate.src.arch_diagram.xml_processor import (
    ArchDiagramXmlProcessor as XmlProcessor,
)
from npu_emulate.src.common.utils import is_in_pyinstaller_env, get_pyinstaller_path


@dataclass
class ViewData:
    name: str
    suffix: str
    metric_groups: tuple


class ArchDiagramView:
    def __init__(self, view_data: ViewData):
        self._viewdata: ViewData = view_data
        self._name: str = view_data.name

    def __str__(self) -> str:
        return f"{self._name}"

    def debug_print_groups(self) -> None:
        for group in self._viewdata.metric_groups:
            log.debug(f"  Group name: {group}")
            group.debug_print_metrics()

    def get_svg_template_path(self, arch) -> str:
        if is_in_pyinstaller_env():
            base_path = get_pyinstaller_path()
        else:
            base_path = os.path.join(os.path.dirname(__file__), "..", "..")
        path_to_template = os.path.abspath(
            os.path.join(
                base_path,
                "static",
                "arch_diagrams",
                f"{arch}",
                f"{self._name}_view.svg",
            )
        )
        return path_to_template

    def process_data(self, input_dir) -> int:
        ret = 0
        for group in self._viewdata.metric_groups:
            ret += group.scan_db(input_dir)
        return ret

    def generate_diagram(self, arch: str, output_dir: str) -> int:
        metrics_dict: Dict[str, str] = defaultdict(str)
        for group in self._viewdata.metric_groups:
            metrics_dict = {**metrics_dict, **group.get_metrics_dict()}

        template_file = self.get_svg_template_path(arch)
        xml_processor = XmlProcessor(
            template_file, output_dir, metrics_dict, self._viewdata.suffix
        )
        return xml_processor.patch_svg_template()
