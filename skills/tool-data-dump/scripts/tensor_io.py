#!/usr/bin/env python3
# -*- coding: UTF-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

"""张量读取与报告存储模块。
用途：读取明确描述的 NPY、NPZ 或裸 bin，并保护已有输出目录；供统计和报告脚本导入。
使用方法：from tensor_io import load_tensor, write_json
单元测试（在 Skill 根目录）：python3 tests/tensor_io_test.py
"""

import json
import math
from pathlib import Path
import tempfile
import zipfile
import zlib


def write_json(path, value):
    write_text_atomic(
        path, json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    )


def write_text_atomic(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix="." + path.name,
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(text)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def fresh_dir(path):
    path = Path(path).resolve()
    if path.exists() and any(path.iterdir()):
        raise ValueError(f"输出目录不是空目录：{path}")
    path.mkdir(parents=True, exist_ok=True)
    return path


def numeric(value):
    value = float(value)
    if math.isnan(value):
        return "NaN"
    if math.isinf(value):
        return "Infinity" if value > 0 else "-Infinity"
    return value


def _load_binary_tensor(path, spec):
    import numpy as np

    if "dtype" not in spec or "shape" not in spec:
        raise ValueError("裸 bin 需要明确 dtype 和 shape；CANN Dump 必须先转换")
    shape = spec["shape"]
    if not isinstance(shape, list):
        raise ValueError("shape 必须为非负整数列表")
    for dimension in shape:
        if (
            not isinstance(dimension, int)
            or isinstance(dimension, bool)
            or dimension < 0
        ):
            raise ValueError("shape 必须为非负整数列表")
    if not isinstance(spec["dtype"], str) or not spec["dtype"]:
        raise ValueError("裸 bin 的 dtype 必须为明确的 NumPy 类型名称")
    dtype = np.dtype(spec["dtype"])
    if dtype.kind not in "biuf":
        raise ValueError("裸 bin 仅支持数值类型")
    if path.stat().st_size != math.prod(shape) * dtype.itemsize:
        raise ValueError("裸 bin 字节数与声明不符")
    return np.fromfile(path, dtype=dtype).reshape(shape)


def _load_tensor_storage(path, spec):
    import numpy as np

    if path.suffix == ".npy":
        return np.lib.format.open_memmap(path, mode="r")
    if path.suffix == ".npz":
        if not isinstance(spec.get("key"), str):
            raise ValueError("NPZ 必须指定 key")
        with path.open("rb") as stream:
            archive = np.load(stream, allow_pickle=False)
            if not isinstance(archive, np.lib.npyio.NpzFile):
                raise ValueError("NPZ 路径实际不包含 NPZ 归档")
            with archive:
                return archive[spec["key"]]
    if path.suffix == ".bin":
        return _load_binary_tensor(path, spec)
    raise ValueError("仅接受转换后的 npy、npz 或元数据明确的裸 bin")


def load_tensor(spec, base):
    import numpy as np

    spec = {"path": spec} if isinstance(spec, str) else spec
    if not isinstance(spec, dict) or not isinstance(spec.get("path"), str):
        raise ValueError("张量描述必须包含 path")
    try:
        array = _load_tensor_storage((base / spec["path"]).resolve(), spec)
    except (EOFError, zipfile.BadZipFile, zlib.error) as error:
        raise ValueError("张量文件不完整或损坏") from error
    if not isinstance(array, np.ndarray):
        raise ValueError("张量内容必须是 NumPy 数组")
    if array.dtype.kind not in "biuf":
        raise ValueError(f"不支持的 dtype：{array.dtype}")
    if array.dtype.kind == "f" and array.dtype.itemsize > 8:
        raise ValueError("浮点比较支持至 float64；未将更高精度 golden 静默降为 float64")
    if "view" in spec:
        raise ValueError("先依据工程元数据确认逻辑布局；本工具不猜测或重建存储视图")
    return array


def read_pairs(path, parse_float=float):
    rows = json.loads(Path(path).read_text(), parse_float=parse_float)
    if not isinstance(rows, list) or not rows:
        raise ValueError("pairs 必须为非空列表")
    if any(not isinstance(row, dict) for row in rows):
        raise ValueError("pairs 每项必须是 JSON 对象")
    labels = [row.get("label") for row in rows]
    if any(not isinstance(label, str) or not label for label in labels):
        raise ValueError("label 必须为非空字符串")
    if len(set(labels)) != len(labels):
        raise ValueError("label 必须唯一")
    return rows
