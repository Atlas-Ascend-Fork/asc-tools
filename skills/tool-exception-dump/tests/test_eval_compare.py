# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import importlib.util
import json
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent
SKILL_ROOT = TEST_ROOT.parent
SCRIPT_PATH = SKILL_ROOT / "scripts" / "eval_compare.py"

SAMPLE_EVAL = {
    "id": 1,
    "title": "测试用例",
    "dimension": "trigger_routing",
    "config": {},
    "prompt": "定位该 AI Core Error",
    "expectations": [
        {"type": "contains", "pattern": "msaicerr"},
        {"type": "not_contains", "pattern": "SHOULD_NOT_APPEAR"},
    ],
}


def _load_module():
    spec = importlib.util.spec_from_file_location("eval_compare", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


eval_compare = _load_module()


def _arm_result(arm_data):
    n_pass, details = eval_compare.score_arm(SAMPLE_EVAL, arm_data)
    return {
        "n_pass": n_pass,
        "n_total": len(details),
        "invalid": bool(arm_data["error"]),
        "wall_time": arm_data["wall_time"],
        "tokens": arm_data["tokens"],
        "error": arm_data["error"],
        "session_id": arm_data["session_id"],
        "details": details,
    }


def _per_case(with_data, without_data):
    return [
        {
            "id": 1,
            "title": SAMPLE_EVAL["title"],
            "dimension": "trigger_routing",
            "arms": {
                "with": _arm_result(with_data),
                "without": _arm_result(without_data),
            },
        }
    ]


def test_invalid_arm_excluded_from_dimension_aggregation():
    with_data = eval_compare.build_mock_arm_data(SAMPLE_EVAL, "with")
    with_data["error"] = "timeout: 进程超过 600s 被终止"
    with_data["ai_text"] = ""
    with_data["full_output"] = ""
    without_data = eval_compare.build_mock_arm_data(SAMPLE_EVAL, "without")

    with_result = _arm_result(with_data)

    # 空回复让 not_contains 真空通过（contains 未命中、not_contains 通过），但该臂已 invalid
    assert with_result["n_pass"] == 1
    assert with_result["invalid"] is True

    dimensions = eval_compare.aggregate_dimensions(
        _per_case(with_data, without_data), ["with", "without"]
    )
    dim = dimensions["trigger_routing"]
    assert dim["with_invalid"] == 1
    assert dim["with_rate"] is None
    assert dim["without_invalid"] == 0
    assert dim["without_rate"] == 0.5


def test_full_mode_with_arm_error_marks_invalid_and_exits_nonzero(
    tmp_path, monkeypatch
):
    def fake_run_single_arm(eval_item, arm, args, skill_dir, out_dir):
        if arm == "with":
            data = eval_compare.build_mock_arm_data(eval_item, "with")
            data["error"] = "timeout: 进程超过 600s 被终止"
            data["ai_text"] = ""
            data["full_output"] = ""
            return data
        return eval_compare.build_mock_arm_data(eval_item, "without")

    monkeypatch.setattr(eval_compare, "run_single_arm", fake_run_single_arm)
    monkeypatch.setattr(eval_compare, "check_full_mode_prereqs", lambda skill_dir: None)
    monkeypatch.setattr(eval_compare, "check_skill_leak", lambda: [])

    out_dir = tmp_path / "out"
    exit_code = eval_compare.main(
        ["--mode", "full", "--arm", "both", "--eval-id", "1", "--out-dir", str(out_dir)]
    )
    assert exit_code == 1

    result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
    with_arm = result["per_case"][0]["arms"]["with"]
    assert with_arm["invalid"] is True
    assert result["meta"]["invalid_arms"]["with"] == 1
    dimensions = result["dimensions"]
    assert dimensions["tool_use_adherence"]["with_invalid"] == 1
    assert dimensions["tool_use_adherence"]["with_rate"] is None

    report = (out_dir / "report.md").read_text(encoding="utf-8")
    assert "invalid" in report
    assert "invalid 臂" in report


def test_full_mode_without_arm_only_failure_keeps_exit_zero(tmp_path, monkeypatch):
    def fake_run_single_arm(eval_item, arm, args, skill_dir, out_dir):
        data = eval_compare.build_mock_arm_data(eval_item, arm)
        if arm == "without":
            data["error"] = "opencode 退出码 1"
            data["ai_text"] = ""
            data["full_output"] = ""
        return data

    monkeypatch.setattr(eval_compare, "run_single_arm", fake_run_single_arm)
    monkeypatch.setattr(eval_compare, "check_full_mode_prereqs", lambda skill_dir: None)
    monkeypatch.setattr(eval_compare, "check_skill_leak", lambda: [])

    out_dir = tmp_path / "out"
    exit_code = eval_compare.main(
        ["--mode", "full", "--arm", "both", "--eval-id", "1", "--out-dir", str(out_dir)]
    )
    assert exit_code == 0

    result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
    assert result["meta"]["invalid_arms"]["without"] == 1
    assert result["meta"]["invalid_arms"]["with"] == 0


def test_dry_run_mock_flow_stays_green_and_exits_zero(tmp_path):
    out_dir = tmp_path / "out"
    exit_code = eval_compare.main(["--mode", "dry-run", "--out-dir", str(out_dir)])
    assert exit_code == 0

    result = json.loads((out_dir / "result.json").read_text(encoding="utf-8"))
    assert result["meta"]["invalid_arms"] == {"with": 0, "without": 0}
    for dim in result["dimensions"].values():
        assert dim["with_invalid"] == 0
        assert dim["without_invalid"] == 0
        assert dim["with_rate"] == 1.0
