#!/usr/bin/python
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

import os
import sys

from setuptools import find_packages

if "--inplace" in sys.argv:
    from distutils.core import setup
else:
    from setuptools import setup

VERSIONS = "0.1.0"

os.environ["SOURCE_DATE_EPOCH"] = str(int(os.path.getctime(os.path.realpath(__file__))))

setup(
    name="npu_emulate",
    version=VERSIONS,
    description="npu_emulate: NPU profiling analyzer and report generator",
    python_requires=">=3.8",
    packages=find_packages(where=".", include=("*",)),
    install_requires=[
        "numpy>=1.21",
        "plotly>=5.0",
    ],
    include_package_data=True,
    package_data={
        "npu_emulate.src.sql": ["*.sql"],
        "npu_emulate.static": ["*.html"],
        "npu_emulate.static.arch_diagrams": ["*.svg"],
        "npu_emulate.static.arch_diagrams.v100": ["*.svg"],
        "npu_emulate.static.top_down": ["*.svg"],
        "npu_emulate.static.top_down.v100": ["*.svg"],
    },
    entry_points={
        "console_scripts": [
            "npu-emulate=npu_emulate.__main__:main",
            "npu-emulate-export-csv=npu_emulate.tools.export_csv:main",
            "npu-emulate-pack-rep=npu_emulate.tools.pack_npu_rep:main",
        ],
    },
)
