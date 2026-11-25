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
"""evals 约束测试。

用途：检查评测用例结构、触发场景覆盖与参考回答的文本断言一致性。
使用方法：python3 -m unittest discover -s tests   （在技能根目录执行）
"""

import copy
import json
from pathlib import Path
import sys
import unittest

_TESTS_DIR = Path(__file__).resolve().parent
if str(_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(_TESTS_DIR))

from package_paths import resolve_roots  # noqa: E402

_, SKILL_ROOT = resolve_roots(__file__)
EVALS_PATH = SKILL_ROOT / "evals" / "evals.json"
SKILL_NAME = "tool-compile-debug"
REVIEW_REQUEST = (
    "请使用宿主的技能工具加载 tool-compile-debug，评审以下 Ascend C 编译修复场景。"
)
AMBIGUOUS_TOOL_BANS = ("不执行工具", "不实际执行工具", "不实际修改或执行工具")

# description 声明的触发词 → 每词必须有正向用例覆盖（prompt/title 按正则命中）
TRIGGER_TERM_PATTERNS = (
    ("CMake", r"CMake"),
    ("头文件", r"头文件|聚合头|include|\.h\b"),
    ("Host/Kernel", r"Host|kernel"),
    ("模板", r"模板|template"),
    ("API 签名", r"API\s*签名|signature|API"),
    ("Tiling", r"[Tt]iling"),
    ("链接", r"链接|undefined|link"),
    ("打包", r"打包|装配"),
    ("SoC", r"SoC|架构门控|门控"),
)
# distractor 只允许取宿主仓真实技能名（防臆造引用）
KNOWN_DISTRACTORS = {
    "knowledge-query",
    "ascendc-api-knowledge-query",
}
ALLOWED_EXPECTATION_TYPES = {"contains", "not_contains", "skill_activated"}
REQUIRED_CASE_KEYS = {
    "id",
    "title",
    "config",
    "prompt",
    "expected_output",
    "files",
    "expectations",
}

# 正确回答可能否定或引用这些错误建议；不能把被否定的子串设为禁项。
REJECTED_ADVICE = {
    5: ("由本技能接管独立运行或精度排查", "编译通过即证明修复完成"),
    8: ("低于下限也完全适用",),
    9: ("编译通过即证明修复完成",),
    12: ("直接修改 kernel 源码",),
    14: ("直接重装 CANN 解决",),
    15: ("跳过前序检查直接重试",),
    16: ("不区分两侧直接修改",),
    17: ("由本技能接管精度排查",),
    18: ("由本技能给出运行时结论",),
}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate evaluation field: {key}")
        result[key] = value
    return result


class EvalsSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = json.loads(
            EVALS_PATH.read_text(encoding="utf-8"), object_pairs_hook=unique_object
        )
        cls.evals = cls.doc["evals"]

    def test_top_level_shape(self):
        self.assertEqual(self.doc["skill_name"], SKILL_NAME)
        self.assertEqual(self.doc["eval_mode"], "text")
        self.assertTrue(self.evals)

    def test_case_keys_and_uniqueness(self):
        ids = [case["id"] for case in self.evals]
        self.assertEqual(len(ids), len(set(ids)))
        for case in self.evals:
            self.assertEqual(set(case), REQUIRED_CASE_KEYS)
            self.assertTrue(case["prompt"].strip(), case["id"])
            self.assertTrue(case["expected_output"].strip(), case["id"])

    def validate_case_contract(self, case):
        self.assertIs(type(case["id"]), int)
        self.assertGreater(case["id"], 0)
        for field in ("title", "prompt", "expected_output"):
            self.assertIsInstance(case[field], str)
            self.assertTrue(case[field].strip())
        self.assertEqual(
            case["files"], [], "Text decision cases must not expose answer files"
        )
        config = case["config"]
        self.assertIsInstance(config, dict)
        self.assertEqual(config["eval_mode"], "text")
        self.assertIs(type(config["max_tokens"]), int)
        self.assertGreater(config["max_tokens"], 0)
        self.assertIsInstance(config["max_tokens_by_model"], dict)
        for model, value in config["max_tokens_by_model"].items():
            self.assertTrue(isinstance(model, str) and model.strip())
            self.assertIs(type(value), int)
            self.assertGreater(value, 0)
        self.assertEqual(set(config["distractor_skills"]), KNOWN_DISTRACTORS)
        self.assertEqual(len(config["distractor_skills"]), len(KNOWN_DISTRACTORS))

    def test_strict_case_contract(self):
        for case in self.evals:
            with self.subTest(case=case["id"]):
                self.validate_case_contract(case)

    def test_rejects_bad_types_budgets_and_duplicate_fields(self):
        for raw in ('{"id":1,"id":2}', '{"config":{"max_tokens":1,"max_tokens":2}}'):
            with self.assertRaises(ValueError):
                json.loads(raw, object_pairs_hook=unique_object)
        mutations = (("id", True), ("id", 0), ("title", ""), ("files", ["answer.json"]))
        for field, value in mutations:
            case = copy.deepcopy(self.evals[0])
            case[field] = value
            with (
                self.subTest(field=field, value=value),
                self.assertRaises(AssertionError),
            ):
                self.validate_case_contract(case)
        for value in (False, 0, -1, "100"):
            case = copy.deepcopy(self.evals[0])
            case["config"]["max_tokens"] = value
            with self.subTest(budget=value), self.assertRaises(AssertionError):
                self.validate_case_contract(case)

    def test_current_knowledge_contract_and_compiler_option(self):
        cases = {case["id"]: case for case in self.evals}
        self.assertIn("旧版1.0.1", cases[2]["prompt"])
        self.assertIn("--npu-arch=dav-3510", cases[10]["prompt"])
        self.assertNotIn("--knowledge-query", json.dumps(self.doc, ensure_ascii=False))
        self.assertIn(
            "knowledge_applicability=requires_card_and_current_sdk_evidence",
            cases[8]["expected_output"],
        )
        for stale in (
            "resolution=ahead",
            "verified_live",
            "ceiling=9.1.0",
            "随包9.0快照",
        ):
            self.assertNotIn(stale, json.dumps(self.doc, ensure_ascii=False))
        # Current instructions and reference answers must name the new
        # provider; explicit references to the old installation stay valid.
        for case in self.evals:
            self.assertNotRegex(
                case["expected_output"], r"(?<!旧)CANNBOT(?!-knowledge)", case["id"]
            )
        current_resources = [
            *(SKILL_ROOT / "references").rglob("*.md"),
            *(SKILL_ROOT / "scripts/engine/harness/config").glob("*.yaml"),
        ]
        for path in current_resources:
            self.assertNotRegex(
                path.read_text(encoding="utf-8"),
                r"(?<!旧)CANNBOT(?!-knowledge)",
                str(path.relative_to(SKILL_ROOT)),
            )
        for ident in (73, 74, 75, 76):
            self.validate_review_setup(cases[ident])

    def test_expectation_types_and_self_activation(self):
        for case in self.evals:
            types = {exp["type"] for exp in case["expectations"]}
            self.assertTrue(types <= ALLOWED_EXPECTATION_TYPES, case["id"])
            activated = [
                exp for exp in case["expectations"] if exp["type"] == "skill_activated"
            ]
            for exp in activated:
                self.assertEqual(exp["pattern"], SKILL_NAME, case["id"])

    def validate_review_setup(self, case):
        """Require a declared skill review, without banning its own loading step."""
        self.assertTrue(case["prompt"].startswith(REVIEW_REQUEST), case["id"])
        self.assertIn(
            {"type": "skill_activated", "pattern": SKILL_NAME},
            case["expectations"],
        )
        for restriction in AMBIGUOUS_TOOL_BANS:
            self.assertNotIn(restriction, case["prompt"], case["id"])

    def test_review_setup_and_activation_are_consistent(self):
        for case in self.evals:
            with self.subTest(case=case["id"]):
                self.validate_review_setup(case)

    def test_review_setup_rejects_missing_context_and_conflicting_tool_ban(self):
        original = self.evals[0]
        for prompt in (
            original["prompt"].removeprefix(REVIEW_REQUEST),
            original["prompt"] + "不执行工具。",
            original["prompt"] + "不实际执行工具。",
        ):
            with self.subTest(prompt=prompt):
                case = dict(original, prompt=prompt)
                with self.assertRaises(AssertionError):
                    self.validate_review_setup(case)

    def test_reference_answers_satisfy_text_expectations(self):
        # contains 不区分大小写；not_contains 是最终回答中的精确子串禁项，
        # 不能把“不做某动作”中的动作词本身设为禁项。
        for case in self.evals:
            reference = case["expected_output"]
            for exp in case["expectations"]:
                with self.subTest(case=case["id"], expectation=exp):
                    pattern = exp["pattern"]
                    self.assertIsInstance(pattern, str)
                    self.assertTrue(pattern.strip())
                    if exp["type"] == "contains":
                        self.assertIn(pattern.lower(), reference.lower())
                    elif exp["type"] == "not_contains":
                        self.assertNotIn(pattern, reference)

    def test_correct_negations_and_quoted_rejections_are_not_forbidden(self):
        for case in self.evals:
            for advice in REJECTED_ADVICE.get(case["id"], ()):
                for rejection in (
                    "不应" + advice,
                    "不能采用以下建议：“" + advice + "”",
                ):
                    response = case["expected_output"] + "\n" + rejection
                    for exp in case["expectations"]:
                        with self.subTest(
                            case=case["id"], rejection=rejection, exp=exp
                        ):
                            if exp["type"] == "contains":
                                self.assertIn(exp["pattern"].lower(), response.lower())
                            elif exp["type"] == "not_contains":
                                self.assertNotIn(exp["pattern"], response)

    def test_contract_review_accepts_equivalent_wording(self):
        # This checks false rejections only. Correctness still requires the
        # independent expected_output review, including its negative controls.
        case = next(case for case in self.evals if case["id"] == 9)
        replacements = (
            {"两种形态": "两个分支"},
            {"声明": "实际接口定义", "重验": "重新按原配方验证"},
            {"两种形态": "这两种情况", "声明": "接口定义", "重验": "复验"},
        )
        for wording in replacements:
            response = case["expected_output"]
            for old, new in wording.items():
                response = response.replace(old, new)
            for exp in case["expectations"]:
                with self.subTest(wording=wording, expectation=exp):
                    if exp["type"] == "contains":
                        self.assertIn(exp["pattern"].lower(), response.lower())
                    elif exp["type"] == "not_contains":
                        self.assertNotIn(exp["pattern"], response)

    def test_boundary_denials_do_not_match_forbidden_actions(self):
        responses = {
            17: "这是精度问题，属于编译修复范围外，不由本技能接管精度排查。",
            18: "这是运行时问题，属于编译修复范围外，不由本技能给出运行时结论。",
        }
        for case in self.evals:
            if case["id"] not in responses:
                continue
            response = responses[case["id"]]
            for exp in case["expectations"]:
                with self.subTest(case=case["id"], exp=exp):
                    if exp["type"] == "contains":
                        self.assertIn(exp["pattern"].lower(), response.lower())
                    elif exp["type"] == "not_contains":
                        self.assertNotIn(exp["pattern"], response)

    def test_out_of_scope_response_cases_exist(self):
        # 职责判读须实际读取被测技能；自然请求的负向触发另在trigger集中。
        cases = {case["id"]: case for case in self.evals}
        for case_id in (17, 18):
            case = cases[case_id]
            self.assertIn(SKILL_NAME, case["prompt"])
            self.assertIn(
                {"type": "skill_activated", "pattern": SKILL_NAME},
                case["expectations"],
            )
            self.assertTrue(
                any(
                    exp["type"] == "contains" and exp["pattern"] in {"精度", "运行"}
                    for exp in case["expectations"]
                ),
                case["id"],
            )
            for exp in case["expectations"]:
                if exp["type"] == "contains":
                    self.assertNotIn(exp["pattern"], KNOWN_DISTRACTORS, case["id"])

    def test_boundary_natural_requests_keep_negative_trigger_coverage(self):
        trigger_doc = json.loads(
            (SKILL_ROOT / "evals" / "trigger-evals.json").read_text(encoding="utf-8")
        )
        triggers = {case["id"]: case for case in trigger_doc["cases"]}
        for case_id, topic in ((17, "精度"), (18, "运行")):
            case = triggers[f"negative_original_text_case_{case_id}"]
            self.assertIs(case["should_trigger"], False)
            self.assertEqual(case["context"], "")
            self.assertIn(topic, case["prompt"])
            self.assertNotIn(SKILL_NAME, case["prompt"])

    def test_positive_activation_cases_exist(self):
        activated = [
            case
            for case in self.evals
            if any(exp["type"] == "skill_activated" for exp in case["expectations"])
        ]
        self.assertGreaterEqual(len(activated), len(self.evals) - 2)

    def test_error_families_covered_by_review_cases(self):
        corpus = "\n".join(
            f"{case['title']}\n{case['prompt']}"
            for case in self.evals
            if any(exp["type"] == "skill_activated" for exp in case["expectations"])
        )
        for term, pattern in TRIGGER_TERM_PATTERNS:
            self.assertRegex(corpus, pattern, f"错误类型 {term} 无决策评审覆盖")

    def test_distractors_are_real_skills(self):
        for case in self.evals:
            for name in case["config"].get("distractor_skills", []):
                self.assertIn(name, KNOWN_DISTRACTORS, case["id"])


if __name__ == "__main__":
    unittest.main()
