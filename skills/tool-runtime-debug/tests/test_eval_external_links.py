# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import json
import re
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent
SKILL_ROOT = TEST_ROOT.parent
REFERENCE_ROOT = SKILL_ROOT / "references"
EVALS_PATH = SKILL_ROOT / "evals" / "evals.json"
EXTERNAL_URL = re.compile(r"https?://[^\s\)]+")
REFERENCE_IN_PROMPT = re.compile(r"references/[A-Za-z0-9._-]+\.md")


def test_every_reference_with_external_links_has_one_guard_eval():
    references = {
        f"references/{path.name}": set(
            EXTERNAL_URL.findall(path.read_text(encoding="utf-8"))
        )
        for path in sorted(REFERENCE_ROOT.glob("*.md"))
    }
    references_with_links = {name: urls for name, urls in references.items() if urls}
    assert references_with_links

    document = json.loads(EVALS_PATH.read_text(encoding="utf-8"))
    guard_evals = [
        item for item in document["evals"] if item["title"].startswith("外链看护：")
    ]
    assert len(guard_evals) == len(references_with_links)

    covered_references = set()
    for item in guard_evals:
        prompt_references = REFERENCE_IN_PROMPT.findall(item["prompt"])
        assert len(prompt_references) == 1, item["title"]
        reference_name = prompt_references[0]
        assert reference_name not in covered_references, reference_name
        covered_references.add(reference_name)

        expectations = item["expectations"]
        assert expectations, item["title"]
        patterns = [entry["pattern"] for entry in expectations]
        assert patterns
        assert all(entry["type"] == "contains" for entry in expectations)
        assert all(
            pattern.startswith("https://www.hiascend.com/") for pattern in patterns
        )
        assert all(
            pattern in references_with_links[reference_name] for pattern in patterns
        )
        assert "不访问外链" in item["prompt"]

    assert covered_references == set(references_with_links)
