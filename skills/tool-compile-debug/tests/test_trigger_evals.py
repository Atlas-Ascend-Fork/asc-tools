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
"""触发评测数据约束测试。

用途：校验数据结构、唯一标识与正反标签；不调用模型或验证宿主选择技能的行为。
使用方法：python3 -m unittest discover -s tests -p test_trigger_evals.py
"""

import copy
import json
import unittest
from pathlib import Path

EVALS_PATH = Path(__file__).resolve().parents[1] / "evals" / "trigger-evals.json"
DOCUMENT_KEYS = {"skill_name", "eval_mode", "cases"}
CASE_KEYS = {"id", "context", "prompt", "should_trigger", "reason", "category"}


def unique_object(pairs):
    """Reject duplicate JSON keys instead of silently replacing a label or input."""
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


class TriggerEvalsSchemaTest(unittest.TestCase):
    """Check evaluation materials; passing these tests is not a model result."""

    @classmethod
    def setUpClass(cls):
        cls.document = json.loads(
            EVALS_PATH.read_text(encoding="utf-8"), object_pairs_hook=unique_object
        )

    def validate_document(self, document):
        self.assertIsInstance(document, dict)
        self.assertEqual(set(document), DOCUMENT_KEYS)
        self.assertEqual(document["skill_name"], "tool-compile-debug")
        self.assertEqual(document["eval_mode"], "skill_trigger")
        cases = document["cases"]
        self.assertIsInstance(cases, list)
        self.assertTrue(cases)
        ids = set()
        inputs = set()
        labels = set()
        for case in cases:
            self.assertIsInstance(case, dict)
            self.assertEqual(set(case), CASE_KEYS)
            for field in ("id", "prompt", "reason", "category"):
                self.assertIsInstance(case[field], str, field)
                self.assertTrue(case[field].strip(), field)
            self.assertIsInstance(case["context"], str)
            self.assertIs(type(case["should_trigger"]), bool)
            self.assertNotIn(case["id"], ids, "Duplicate case ID")
            ids.add(case["id"])
            case_input = (case["context"].strip(), case["prompt"].strip())
            self.assertNotIn(case_input, inputs, "Duplicate evaluation input")
            inputs.add(case_input)
            labels.add(case["should_trigger"])
        self.assertEqual(labels, {False, True}, "Both trigger labels are required")

    def test_schema_and_both_labels(self):
        self.validate_document(self.document)

    def test_rejects_invalid_document_metadata(self):
        mutations = (
            ("skill_name", "other-skill"),
            ("eval_mode", "text"),
            ("cases", {}),
            ("cases", []),
        )
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                document = copy.deepcopy(self.document)
                document[field] = value
                with self.assertRaises(AssertionError):
                    self.validate_document(document)

    def test_rejects_missing_or_unknown_case_fields(self):
        for field in CASE_KEYS:
            with self.subTest(missing=field):
                document = copy.deepcopy(self.document)
                del document["cases"][0][field]
                with self.assertRaises(AssertionError):
                    self.validate_document(document)
        document = copy.deepcopy(self.document)
        document["cases"][0]["expected_output"] = "Not a trigger label"
        with self.assertRaises(AssertionError):
            self.validate_document(document)

    def test_rejects_invalid_text_fields(self):
        for field in ("id", "prompt", "reason", "category"):
            for value in (None, 7, "", "   "):
                with self.subTest(field=field, value=value):
                    document = copy.deepcopy(self.document)
                    document["cases"][0][field] = value
                    with self.assertRaises(AssertionError):
                        self.validate_document(document)
        document = copy.deepcopy(self.document)
        document["cases"][0]["context"] = None
        with self.assertRaises(AssertionError):
            self.validate_document(document)

    def test_rejects_non_boolean_labels(self):
        for value in (None, 0, 1, "true", "false"):
            with self.subTest(value=value):
                document = copy.deepcopy(self.document)
                document["cases"][0]["should_trigger"] = value
                with self.assertRaises(AssertionError):
                    self.validate_document(document)

    def test_rejects_duplicate_ids_and_inputs(self):
        document = copy.deepcopy(self.document)
        document["cases"][1]["id"] = document["cases"][0]["id"]
        with self.assertRaises(AssertionError):
            self.validate_document(document)
        document = copy.deepcopy(self.document)
        for field in ("context", "prompt"):
            document["cases"][1][field] = document["cases"][0][field]
        with self.assertRaises(AssertionError):
            self.validate_document(document)

    def test_rejects_a_missing_label_class(self):
        for label in (False, True):
            with self.subTest(label=label):
                document = copy.deepcopy(self.document)
                for case in document["cases"]:
                    case["should_trigger"] = label
                with self.assertRaises(AssertionError):
                    self.validate_document(document)

    def test_rejects_duplicate_json_keys(self):
        with self.assertRaises(ValueError):
            json.loads(
                '{"should_trigger": true, "should_trigger": false}',
                object_pairs_hook=unique_object,
            )

    def test_allows_natural_language_revisions(self):
        document = copy.deepcopy(self.document)
        document["cases"][0]["prompt"] = "Please diagnose this Ascend C build failure."
        document["cases"][0]["reason"] = "The current request concerns a failed build."
        document["cases"][0]["category"] = "build_diagnosis"
        self.validate_document(document)

    def test_positive_discovery_does_not_explicitly_name_the_skill(self):
        # A requested skill review is tested in evals.json, not discovery recall.
        for case in self.document["cases"]:
            if case["should_trigger"]:
                with self.subTest(case=case["id"]):
                    self.assertNotIn(
                        self.document["skill_name"],
                        case["context"] + "\n" + case["prompt"],
                    )


if __name__ == "__main__":
    unittest.main()
