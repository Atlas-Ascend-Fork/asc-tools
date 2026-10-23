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

"""tensor_io.py 单元测试。
用途：核验该脚本的数据行为、边界与错误处理。
使用方法（在 Skill 根目录）：python3 tests/tensor_io_test.py
"""

import importlib
from decimal import Decimal
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
tensor_io = importlib.import_module("tensor_io")


class TensorIoTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.array = np.array([[1, 2], [3, 4]], np.float16)
        np.save(self.root / "golden.npy", self.array)

    def test_npy_keeps_shape_dtype_and_relative_path(self):
        actual = tensor_io.load_tensor("golden.npy", self.root)
        np.testing.assert_array_equal(actual, self.array)
        self.assertEqual(actual.shape, (2, 2))
        self.assertEqual(actual.dtype, np.dtype("float16"))

    def test_npz_requires_explicit_key(self):
        np.savez(self.root / "golden.npz", a=self.array, b=self.array * 2)
        with self.assertRaises(ValueError):
            tensor_io.load_tensor("golden.npz", self.root)
        np.testing.assert_array_equal(
            tensor_io.load_tensor({"path": "golden.npz", "key": "b"}, self.root),
            self.array * 2,
        )

    def test_raw_binary_uses_explicit_dtype_and_shape(self):
        self.array.tofile(self.root / "golden.bin")
        spec = {"path": "golden.bin", "dtype": "float16", "shape": [2, 2]}
        actual = tensor_io.load_tensor(spec, self.root)
        np.testing.assert_array_equal(actual, self.array)
        self.assertEqual(actual.dtype, self.array.dtype)
        for key in ("dtype", "shape"):
            with self.subTest(missing=key), self.assertRaises(ValueError):
                tensor_io.load_tensor(
                    {name: value for name, value in spec.items() if name != key},
                    self.root,
                )

    def test_raw_binary_rejects_invalid_shape_or_byte_count(self):
        self.array.tofile(self.root / "golden.bin")
        for shape in ([4, 2], [-1, 4], [True, 4], [2.0, 2], ["2", 2], (2, 2)):
            with self.subTest(shape=shape), self.assertRaises(ValueError):
                tensor_io.load_tensor(
                    {"path": "golden.bin", "dtype": "float16", "shape": shape},
                    self.root,
                )

    def test_raw_binary_requires_an_explicit_dtype_name(self):
        self.array.tofile(self.root / "golden.bin")
        for dtype in (None, "", False, {}):
            with self.subTest(dtype=dtype), self.assertRaises(ValueError):
                tensor_io.load_tensor(
                    {"path": "golden.bin", "dtype": dtype, "shape": [2, 2]}, self.root
                )

    def test_tensor_rejects_unsupported_dtype_and_implicit_view(self):
        np.save(self.root / "text.npy", np.array(["not numeric"]))
        for spec in ("text.npy", {"path": "golden.npy", "view": [4]}):
            with self.subTest(spec=spec), self.assertRaises(ValueError):
                tensor_io.load_tensor(spec, self.root)

    def test_corrupt_archive_and_wrong_storage_type_are_input_errors(self):
        np.savez(self.root / "archive.npz", a=self.array)
        payload = (self.root / "archive.npz").read_bytes()
        (self.root / "broken.npz").write_bytes(payload[:40])
        (self.root / "fake.npy").write_bytes(payload)
        (self.root / "fake.npz").write_bytes((self.root / "golden.npy").read_bytes())
        with zipfile.ZipFile(self.root / "bytes.npz", "w") as archive:
            archive.writestr("a", "not an array")
        for spec in (
            {"path": "broken.npz", "key": "a"},
            "fake.npy",
            {"path": "fake.npz", "key": "a"},
            {"path": "bytes.npz", "key": "a"},
        ):
            with self.subTest(spec=spec), self.assertRaises(ValueError):
                tensor_io.load_tensor(spec, self.root)

    def test_pairs_preserve_distinct_calls_and_reject_invalid_labels(self):
        path = self.root / "pairs.json"
        rows = [{"label": "request0/y"}, {"label": "request1/y"}]
        tensor_io.write_json(path, rows)
        self.assertEqual(tensor_io.read_pairs(path), rows)
        for invalid in (
            [],
            [None],
            [1],
            [[]],
            [{}],
            [{"label": ""}],
            [rows[0], rows[0]],
        ):
            with self.subTest(rows=invalid), self.assertRaises(ValueError):
                tensor_io.write_json(path, invalid)
                tensor_io.read_pairs(path)

    def test_pairs_can_preserve_decimal_threshold_tokens(self):
        path = self.root / "pairs.json"
        path.write_text(
            '[{"label":"r/y","criterion":{"atol":9007199254740993.9,'
            '"rtol":9.0071992547409939e-16}}]'
        )
        criterion = tensor_io.read_pairs(path, parse_float=Decimal)[0]["criterion"]
        self.assertEqual(criterion["atol"], Decimal("9007199254740993.9"))
        self.assertEqual(criterion["rtol"], Decimal("9.0071992547409939e-16"))

    def test_nonfinite_values_have_portable_json_representation(self):
        values = [np.nan, np.inf, -np.inf, 0.25]
        report = {"values": [tensor_io.numeric(value) for value in values]}
        path = self.root / "report/summary.json"
        tensor_io.write_json(path, report)
        self.assertEqual(
            json.loads(path.read_text()),
            {"values": ["NaN", "Infinity", "-Infinity", 0.25]},
        )
        with self.assertRaises(ValueError):
            tensor_io.write_json(self.root / "invalid.json", {"raw": np.nan})

    def test_fresh_directory_preserves_existing_results(self):
        out = self.root / "results"
        self.assertEqual(tensor_io.fresh_dir(out), out.resolve())
        self.assertEqual(tensor_io.fresh_dir(out), out.resolve())
        saved = out / "report.md"
        saved.write_text("keep")
        with self.assertRaises(ValueError):
            tensor_io.fresh_dir(out)
        self.assertEqual(saved.read_text(), "keep")

    def test_failed_atomic_update_preserves_previous_json(self):
        path = self.root / "summary.json"
        tensor_io.write_json(path, {"completed": 2})
        before = path.read_bytes()
        with patch.object(Path, "replace", side_effect=OSError("interrupted replace")):
            with self.assertRaises(OSError):
                tensor_io.write_json(path, {"completed": 3})
        self.assertEqual(path.read_bytes(), before)
        self.assertFalse(list(self.root.glob(".summary.json*")))


if __name__ == "__main__":
    unittest.main()
