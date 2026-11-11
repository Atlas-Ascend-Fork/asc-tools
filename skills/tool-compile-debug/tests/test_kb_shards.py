#!/usr/bin/env python3
# -*- coding: UTF-8 -*-
# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""知识库形态测试。

用途：钉住包内知识库的发布形态——api_signatures 以单文件或分片发布、每片不超
      过宿主仓单文件大小限制、全部配置可解析。
使用方法：python3 -m unittest discover -s tests   （在技能根目录执行）
"""

import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

try:
    import yaml
except Exception:  # pragma: no cover - 目标环境应有 PyYAML
    yaml = None

SKILL_ROOT = Path(__file__).resolve().parents[1]
CONFIGS = SKILL_ROOT / "scripts" / "engine" / "harness/knowledge"
# 宿主仓单文件大小门禁为 500KB，分片阈值留出余量
SHARD_LIMIT_BYTES = 500 * 1024


def load_common_module():
    path = SKILL_ROOT / "scripts" / "engine" / "harness/engine" / "common.py"
    spec = importlib.util.spec_from_file_location("tool_compile_debug_common", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


COMMON = load_common_module()


@unittest.skipIf(yaml is None, "PyYAML 不可用")
class KbShardTest(unittest.TestCase):
    def test_api_signatures_ships_single_or_sharded(self):
        single = CONFIGS / "api_signatures.yaml"
        parts = sorted((CONFIGS / "api_signatures").glob("part-*.yaml"))
        self.assertTrue(
            single.is_file() or parts,
            "api_signatures 必须以单文件或 part-*.yaml 分片之一发布",
        )

    def test_shards_within_size_gate(self):
        parts = sorted((CONFIGS / "api_signatures").glob("part-*.yaml"))
        for part in parts:
            self.assertLess(
                part.stat().st_size,
                SHARD_LIMIT_BYTES,
                f"{part.name} 超过单文件大小门禁",
            )

    def test_api_signature_entries_parse_with_names(self):
        entries = COMMON.load_api_signature_kb(CONFIGS)
        self.assertGreater(len(entries), 1500)
        for entry in entries:
            self.assertIn("name", entry)

    def test_required_configs_parse(self):
        for name in (
            "cann_error_codes",
            "error_signatures",
            "fix_hints",
            "fix_templates",
            "soc_feature_matrix",
            "static_rules",
            "versions",
        ):
            payload = yaml.safe_load(
                (CONFIGS / f"{name}.yaml").read_text(encoding="utf-8")
            )
            self.assertIsNotNone(payload, name)


@unittest.skipIf(yaml is None, "PyYAML 不可用")
class KbIntegrityTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="compile-debug-kb-")
        self.addCleanup(temporary.cleanup)
        self.configs = Path(temporary.name)
        self.shards = self.configs / "api_signatures"
        self.shards.mkdir()
        self.manifest = {
            "schema_version": "api-signatures-shards-v1",
            "entry_count": 2,
            "parts": [],
        }
        for index, name in enumerate(("FirstApi", "SecondApi"), start=1):
            content = f"- name: {name}\n".encode("utf-8")
            part_name = f"part-{index:04d}.yaml"
            (self.shards / part_name).write_bytes(content)
            self.manifest["parts"].append(
                {
                    "name": part_name,
                    "entry_count": 1,
                    "sha256": hashlib.sha256(content).hexdigest(),
                }
            )
        self.write_manifest()

    def write_manifest(self):
        (self.shards / "manifest.json").write_text(
            json.dumps(self.manifest), encoding="utf-8"
        )

    def test_complete_shards_preserve_declared_entry_order(self):
        entries = COMMON.load_api_signature_kb(self.configs)
        self.assertEqual(
            [entry["name"] for entry in entries], ["FirstApi", "SecondApi"]
        )

    def test_missing_trailing_shard_is_rejected(self):
        (self.shards / "part-0002.yaml").unlink()
        with self.assertRaisesRegex(COMMON.KnowledgeBaseIntegrityError, "missing="):
            COMMON.load_api_signature_kb(self.configs)

    def test_extra_shard_is_rejected(self):
        (self.shards / "part-0003.yaml").write_text(
            "- name: ExtraApi\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(COMMON.KnowledgeBaseIntegrityError, "unexpected="):
            COMMON.load_api_signature_kb(self.configs)

    def test_missing_manifest_is_rejected(self):
        (self.shards / "manifest.json").unlink()
        with self.assertRaisesRegex(COMMON.KnowledgeBaseIntegrityError, "manifest"):
            COMMON.load_api_signature_kb(self.configs)

    def test_modified_shard_is_rejected(self):
        (self.shards / "part-0001.yaml").write_text(
            "- name: ChangedApi\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(COMMON.KnowledgeBaseIntegrityError, "checksum"):
            COMMON.load_api_signature_kb(self.configs)

    def test_invalid_yaml_or_entry_shape_is_rejected_even_with_matching_hash(self):
        for content in (b"broken: [\n", b"name: NotAList\n", b"- NotAnObject\n"):
            with self.subTest(content=content):
                (self.shards / "part-0001.yaml").write_bytes(content)
                self.manifest["parts"][0]["sha256"] = hashlib.sha256(
                    content
                ).hexdigest()
                self.write_manifest()
                with self.assertRaises(COMMON.KnowledgeBaseIntegrityError):
                    COMMON.load_api_signature_kb(self.configs)

    def test_wrong_count_or_order_is_rejected(self):
        original = json.dumps(self.manifest)
        for mutation in ("total_count", "part_count", "part_order", "unsafe_name"):
            with self.subTest(mutation=mutation):
                self.manifest = json.loads(original)
                if mutation == "total_count":
                    self.manifest["entry_count"] += 1
                elif mutation == "part_count":
                    self.manifest["parts"][0]["entry_count"] += 1
                elif mutation == "part_order":
                    self.manifest["parts"].reverse()
                else:
                    self.manifest["parts"][0]["name"] = "../outside.yaml"
                self.write_manifest()
                with self.assertRaises(COMMON.KnowledgeBaseIntegrityError):
                    COMMON.load_api_signature_kb(self.configs)


if __name__ == "__main__":
    unittest.main()
