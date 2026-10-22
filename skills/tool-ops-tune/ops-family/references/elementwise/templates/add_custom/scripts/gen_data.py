# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------


# ============================================================================
# 测试数据生成脚本
# ============================================================================
#
# [MODIFY] 创建新算子时修改:
# 1. total_length: 总元素数（需与 add.asc 中的保持一致）
# 2. dtype: 数据类型（需与 add.asc 中的保持一致）
# 3. 输入数据: 根据你的算子生成对应的输入
# 4. golden 计算: 修改 golden.py 中的 compute_golden()
# ============================================================================

import argparse
import os

import numpy as np

from golden import compute_golden

BIN_EXT = "." + "bin"  # 数据文件扩展名
parser = argparse.ArgumentParser(description="add_custom 测试数据生成")
parser.add_argument(
    "--input-x",
    default=os.path.join("input", "input_x" + BIN_EXT),
    help="x 输入文件路径",
)
parser.add_argument(
    "--input-y",
    default=os.path.join("input", "input_y" + BIN_EXT),
    help="y 输入文件路径",
)
parser.add_argument(
    "--golden",
    default=os.path.join("output", "golden" + BIN_EXT),
    help="golden 输出路径",
)
args = parser.parse_args()

os.makedirs("input", exist_ok=True)
os.makedirs("output", exist_ok=True)

# [MODIFY] 数据规格参数
total_length = 4096 * 8  # 总元素数，需与 add.asc 中 main 函数保持一致
dtype = np.float32  # 数据类型，需与 add.asc 中模板参数保持一致

# [MODIFY] 生成输入数据（根据你的算子调整）
x = np.random.randn(total_length).astype(dtype)
y = np.random.randn(total_length).astype(dtype)

x.tofile(args.input_x)
y.tofile(args.input_y)

# [MODIFY] 计算 golden（逻辑定义在 golden.py 中，修改那里即可）
golden = compute_golden(x, y)
golden.tofile(args.golden)

print(f"Generated test data: {total_length} elements, dtype={dtype}")
print(f"  {args.input_x}: {x.shape}, {x.dtype}")
print(f"  {args.input_y}: {y.shape}, {y.dtype}")
print(f"  {args.golden}: {golden.shape}, {golden.dtype}")
