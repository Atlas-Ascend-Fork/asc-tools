# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import copy
import json
import unittest
from pathlib import Path


EVALS_PATH = Path(__file__).resolve().parents[1] / "evals" / "evals.json"
REPORT_TEMPLATE_PATH = (
    Path(__file__).resolve().parents[1] / "assets" / "analysis-report-template.md"
)

REQUIRED_CASE_KEYS = {
    "id",
    "title",
    "config",
    "prompt",
    "expected_output",
    "files",
    "expectations",
}
ALLOWED_EXPECTATION_TYPES = {"contains", "not_contains"}
PROMPT_FACTS = {
    1: ("3.640416",),
    2: ("aiv_mte2_active_bw", "aiv_mte3_active_bw"),
    4: ("3.430269", "3.891671"),
    5: ("Ascend950PR_9599 V100", "62061.89"),
    6: ("62063.16", "62061.89"),
    8: ("0.097392", "0.096824"),
    9: ("0.002268", "0.730649"),
    10: ("displayTimeUnit", "SCALAR", "VECTOR", "MTE2"),
    11: ("displayTimeUnit", '"ts":2.5', '"dur":0.25'),
    12: ("process2.result3.device0.replay1.group4.veccore1",),
    13: ("traceEvents", '"ts":1.0', '"dur":2.0'),
    14: ("./add_custom", "Memory"),
    15: ("run.sh", "Memory", "L2Cache", "--input input.bin", "--repeat 2"),
    16: ("场景 A", "场景 B", "/work/add", "./add_custom"),
    17: ("/tmp/input.npu-rep", "/tmp/import-parent"),
    18: (
        "/tmp/import-parent/npu-compute-import-123",
        "collection-p42-0001",
        "unknown.bin",
    ),
    19: ("summary.jsonl", '"category":"Memory"', '"category":"OpInfoSummary"'),
    20: ("group0.veccore0", "group5.cubecore"),
    21: ("./add_custom",),
    22: ("./add_custom",),
    23: ("./add_custom",),
    24: ("./add_custom",),
    25: ("./add_custom",),
    27: ("./add_custom",),
    29: ("./add_custom", "--input input.bin"),
}
PIPELINE_EVAL_TITLES = {
    "Pipeline 时间线汇总",
    "Pipeline 时间单位解释",
    "Pipeline 合并轨道来源",
    "Pipeline 空数据与重叠事件边界",
}
WORKFLOW_EVAL_TITLES = {
    "生成单 Section 采集命令",
    "生成多 Section 脚本采集命令",
    "区分询问方法和直接执行",
    "解包 npu-rep 报告",
    "检查完整解包结果",
    "summary 数据解释",
    "Pipeline 采样边界解释",
}


REQUIRED_SCENARIOS = {
    21: {
        "title": "常用指标 Set 选择",
        "prompt": ("采集常用指标",),
        "expected": ("--set basic", "basic"),
    },
    22: {
        "title": "完整指标 Set 选择",
        "prompt": ("采集全部指标",),
        "expected": ("--set full", "full"),
    },
    23: {
        "title": "未指定指标使用默认 basic",
        "prompt": ("只采集", "./add_custom"),
        "expected": ("npu-compute ./add_custom", "默认", "basic"),
    },
    24: {
        "title": "Section 先于 basic Set 的组合顺序",
        "prompt": ("先采集 Memory", "再补充 basic"),
        "expected": ("--section Memory --set basic", "首次", "去重"),
    },
    25: {
        "title": "basic Set 追加 ResourceConflictRatio",
        "prompt": ("basic", "ResourceConflictRatio"),
        "expected": ("--set basic --section ResourceConflictRatio", "full"),
    },
    26: {
        "title": "独立查询支持的 Set",
        "prompt": ("支持哪些集合", "查看"),
        "expected": ("npu-compute --list-sets", "不附加", "目标程序"),
    },
    27: {
        "title": "单 Section 不扩大采集范围",
        "prompt": ("单个 Section", "Memory"),
        "expected": ("--section Memory", "不扩大"),
    },
    28: {
        "title": "Set 采集后的报告分析流程",
        "prompt": ("--set full", "报告"),
        "expected": ("report", "解包", "结果解析"),
    },
    29: {
        "title": "程序参数使用显式分隔符",
        "prompt": ("程序参数", "--input"),
        "expected": ("--set basic --", "目标程序", "程序参数"),
    },
}


class SkillEvalsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.document = json.loads(EVALS_PATH.read_text(encoding="utf-8"))
        cls.data = cls.document
        cls.evals = cls.document["evals"]
        cls.evals_by_id = {item["id"]: item for item in cls.evals}
        cls.report_template = REPORT_TEMPLATE_PATH.read_text(encoding="utf-8")

    def test_top_level_uses_text_evaluation_mode(self):
        self.assertEqual(self.document["skill_name"], "tool-npu-compute")
        self.assertEqual(self.document.get("eval_mode"), "text")

    def test_case_ids_are_unique_and_sequential(self):
        ids = [case["id"] for case in self.evals]
        self.assertEqual(ids, list(range(1, len(ids) + 1)))

    def test_cases_follow_cannbot_schema(self):
        for case in self.evals:
            with self.subTest(case_id=case["id"]):
                self.assertTrue(REQUIRED_CASE_KEYS.issubset(case))
                self.assertNotIn("name", case)
                self.assertIsInstance(case["title"], str)
                self.assertTrue(case["title"].strip())
                self.assertIsInstance(case["config"], dict)
                self.assertEqual(case["config"]["eval_mode"], "text")
                self.assertGreater(case["config"]["max_tokens"], 0)

    def test_expectations_are_machine_readable(self):
        for case in self.evals:
            with self.subTest(case_id=case["id"]):
                self.assertTrue(case["expectations"])
                for expectation in case["expectations"]:
                    self.assertIsInstance(expectation, dict)
                    self.assertEqual(
                        set(expectation), {"type", "pattern", "description"}
                    )
                    self.assertIn(expectation["type"], ALLOWED_EXPECTATION_TYPES)
                    self.assertTrue(expectation["pattern"])
                    self.assertTrue(expectation["description"])

    def test_evaluation_inputs_are_self_contained(self):
        for case in self.evals:
            with self.subTest(case_id=case["id"]):
                self.assertEqual(case["files"], [])
                for fact in PROMPT_FACTS.get(case["id"], ()):
                    self.assertIn(fact, case["prompt"])

    def test_pipeline_evaluations_cover_timeline_analysis_boundaries(self):
        pipeline_cases = [
            case for case in self.evals if case["title"] in PIPELINE_EVAL_TITLES
        ]
        self.assertEqual(
            {case["title"] for case in pipeline_cases}, PIPELINE_EVAL_TITLES
        )
        combined_expected_output = "\n".join(
            case["expected_output"] for case in pipeline_cases
        )
        for fact in (
            "微秒",
            "displayTimeUnit",
            "process",
            "result",
            "device",
            "replay",
            "group",
            "空时间线",
            "Kernel 时长",
        ):
            with self.subTest(fact=fact):
                self.assertIn(fact, combined_expected_output)

    def test_report_template_supports_pipeline_timeline_evidence(self):
        for content in (
            "## Pipeline 时间线",
            "时间范围（us）",
            "事件数",
            "轨道数",
            "流水线",
            "轨道标识",
            "累计事件时长（us）",
            "Kernel 时长",
        ):
            with self.subTest(content=content):
                self.assertIn(content, self.report_template)

    def test_workflow_evaluations_cover_natural_language_usage(self):
        workflow_cases = [
            case for case in self.evals if case["title"] in WORKFLOW_EVAL_TITLES
        ]
        self.assertEqual(
            {case["title"] for case in workflow_cases}, WORKFLOW_EVAL_TITLES
        )
        combined_prompts = "\n".join(case["prompt"] for case in workflow_cases)
        combined_expected = "\n".join(
            case["expected_output"] for case in workflow_cases
        )
        for fact in (
            "--section Memory",
            "--section L2Cache",
            "bash run.sh",
            "只告诉我命令",
            "请直接执行",
            "--import",
            "inspect_collection.py",
            "summary.jsonl",
            "最多 6 个采样 Group",
        ):
            with self.subTest(fact=fact):
                self.assertIn(fact, combined_prompts + combined_expected)

    def test_report_template_records_end_to_end_evidence(self):
        for content in (
            "## 执行信息",
            "用户意图",
            "工作目录",
            "执行命令",
            "退出状态",
            "报告路径",
            "解包目录",
            "## 解包结果",
            "子结果集合",
            "未知项目",
            "## summary 摘要",
            "采样 Group",
            "Core 轨道",
        ):
            with self.subTest(content=content):
                self.assertIn(content, self.report_template)

    def test_set_scenarios_cover_required_natural_language_contracts(self):
        for eval_id, scenario in REQUIRED_SCENARIOS.items():
            with self.subTest(eval_id=eval_id):
                self.assertIn(eval_id, self.evals_by_id)
                item = self.evals_by_id[eval_id]
                self.assertEqual(item["title"], scenario["title"])
                for fragment in scenario["prompt"]:
                    self.assertIn(fragment, item["prompt"])
                for fragment in scenario["expected"]:
                    self.assertIn(fragment, item["expected_output"])

    def test_set_eval_ids_are_contiguous_extension(self):
        self.assertEqual(
            sorted(eval_id for eval_id in self.evals_by_id if eval_id >= 21),
            list(range(21, 30)),
        )

    def test_set_scenarios_enforce_command_boundaries(self):
        expected_outputs = {
            23: "npu-compute ./add_custom",
            26: "npu-compute --list-sets",
            28: "--import <report.npu-rep> --export <目录>",
            29: "npu-compute --set basic -- ./add_custom --input input.bin",
        }
        for eval_id, fragment in expected_outputs.items():
            with self.subTest(eval_id=eval_id):
                self.assertIn(fragment, self.evals_by_id[eval_id]["expected_output"])

        command_line_23 = self.evals_by_id[23]["expected_output"].split("。", 1)[0]
        self.assertNotIn("--set", command_line_23)
        self.assertNotIn("--section", command_line_23)
        self.assertIn("不生成采集报告", self.evals_by_id[26]["expected_output"])
        for fragment in ("退出状态", "HardwareInfo", "summary", "Section CSV"):
            self.assertIn(fragment, self.evals_by_id[28]["expected_output"])
        self.assertIn(
            "后续内容全部作为程序参数传递", self.evals_by_id[29]["expected_output"]
        )


TRIGGER_SCENARIOS = {
    "positive_tool_name": (True, "tool_name"),
    "positive_case_variant": (True, "tool_name"),
    "positive_report": (True, "artifact"),
    "positive_trace": (True, "artifact"),
    "positive_section": (True, "metric"),
    "negative_directory_analysis": (False, "directory"),
    "negative_memory_analysis": (False, "domain_intent"),
    "positive_cache": (True, "domain_intent"),
    "negative_ascend_context": (False, "context"),
    "positive_directory_tool": (True, "directory"),
    "positive_memory_collection": (True, "domain_intent"),
    "negative_performance_only": (False, "intent_boundary"),
    "negative_ascend_optimization": (False, "intent_boundary"),
    "positive_existing_csv": (True, "context"),
    "negative_python_set": (False, "generic_term"),
    "negative_ci_pipeline": (False, "generic_term"),
    "negative_network": (False, "other_domain"),
    "negative_csv": (False, "generic_term"),
    "negative_web": (False, "other_domain"),
    "negative_compile": (False, "intent_boundary"),
    "negative_transcript": (False, "incidental_mention"),
    "negative_section": (False, "generic_term"),
    "negative_memory": (False, "generic_term"),
    "negative_context": (False, "context"),
    "positive_communication_trace": (True, "artifact"),
    "negative_generic_roofline": (False, "other_domain"),
}


class SkillTriggerEvalsTest(unittest.TestCase):
    """Validate evaluation data, not the host's skill-selection behavior."""

    @classmethod
    def setUpClass(cls):
        cls.document = json.loads(
            EVALS_PATH.with_name("trigger-evals.json").read_text(encoding="utf-8")
        )

    def validate_document(self, document):
        self.assertIsInstance(document, dict)
        self.assertEqual(document.get("skill_name"), "tool-npu-compute")
        self.assertEqual(document.get("eval_mode"), "skill_trigger")
        cases = document.get("cases")
        self.assertIsInstance(cases, list)
        categories = {category for _, category in TRIGGER_SCENARIOS.values()}
        by_id = {}
        for case in cases:
            self.assertIsInstance(case, dict)
            for key in ("id", "prompt", "reason", "category"):
                self.assertIsInstance(case.get(key), str, key)
                self.assertTrue(case.get(key).strip(), key)
            self.assertIsInstance(case.get("context"), str)
            self.assertIs(type(case.get("should_trigger")), bool)
            self.assertIn(case.get("category"), categories)
            self.assertNotIn(case.get("id"), by_id, "duplicate ID")
            by_id[case.get("id")] = case
        self.assertTrue(TRIGGER_SCENARIOS.keys() <= by_id.keys(), "missing scenarios")
        for case_id, (expected, category) in TRIGGER_SCENARIOS.items():
            case = by_id[case_id]
            self.assertIs(case.get("should_trigger"), expected, case_id)
            self.assertEqual(case.get("category"), category, case_id)
            if category == "context":
                self.assertTrue(case.get("context", "").strip(), case_id)

    def test_trigger_data_schema_and_required_coverage(self):
        self.validate_document(self.document)

    def test_rejects_invalid_case_fields(self):
        mutations = []
        for key in ("id", "context", "prompt", "should_trigger", "reason", "category"):
            mutations.append((key, None))
        mutations += [
            ("id", " "),
            ("prompt", " "),
            ("reason", " "),
            ("category", "unknown"),
            ("should_trigger", "false"),
            ("should_trigger", 1),
            ("context", []),
        ]
        for key, value in mutations:
            with self.subTest(key=key, value=value):
                document = copy.deepcopy(self.document)
                if value is None:
                    del document["cases"][0][key]
                else:
                    document["cases"][0][key] = value
                with self.assertRaises(AssertionError):
                    self.validate_document(document)

    def test_rejects_invalid_document_metadata(self):
        for key, value in (
            ("skill_name", "other"),
            ("eval_mode", "text"),
            ("cases", {}),
            ("cases", []),
        ):
            with self.subTest(key=key, value=value):
                document = copy.deepcopy(self.document)
                document[key] = value
                with self.assertRaises(AssertionError):
                    self.validate_document(document)

    def test_rejects_duplicate_ids(self):
        document = copy.deepcopy(self.document)
        document["cases"].append(copy.deepcopy(document["cases"][0]))
        with self.assertRaisesRegex(AssertionError, "duplicate ID"):
            self.validate_document(document)

    def test_rejects_missing_scenarios(self):
        for case_id in TRIGGER_SCENARIOS:
            with self.subTest(case_id=case_id):
                document = copy.deepcopy(self.document)
                document["cases"] = [
                    case for case in document["cases"] if case["id"] != case_id
                ]
                with self.assertRaisesRegex(AssertionError, "missing scenarios"):
                    self.validate_document(document)

    def test_rejects_missing_negative_cases(self):
        document = copy.deepcopy(self.document)
        document["cases"] = [
            case for case in document["cases"] if case["should_trigger"]
        ]
        with self.assertRaisesRegex(AssertionError, "missing scenarios"):
            self.validate_document(document)

    def test_rejects_changed_expectations_categories_and_context(self):
        for index, case in enumerate(self.document["cases"]):
            changes = [("should_trigger", not case["should_trigger"])]
            changes.append(
                ("category", "context" if case["category"] != "context" else "metric")
            )
            if case["category"] == "context":
                changes.append(("context", " "))
            for key, value in changes:
                self.assert_case_mutation_rejected(index, case["id"], key, value)

    def assert_case_mutation_rejected(self, index, case_id, key, value):
        with self.subTest(case_id=case_id, key=key):
            document = copy.deepcopy(self.document)
            document["cases"][index][key] = value
            with self.assertRaises(AssertionError):
                self.validate_document(document)


if __name__ == "__main__":
    unittest.main()
