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
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent
SKILL_ROOT = TEST_ROOT.parent
SKILL_PATH = SKILL_ROOT / "SKILL.md"
EVALS_PATH = SKILL_ROOT / "evals" / "evals.json"

POSITIVE_TRIGGER_TITLES = {
    "ACL/ACLNN 返回码异常": "触发：ACL/ACLNN 返回码异常",
    "输出参数或读回数据错误": "输出所有权：descriptor 与读回地址不一致",
    "Context/Stream/Event 顺序可疑": "Event 时序：wait 与 record 顺序可疑",
    "Memcpy 容量或方向不符": "Memcpy 容量：目的缓冲区小于请求字节数",
    "异步完成后读取异常": "异步生命周期：Host 缓冲区可能提前失效",
    "资源重复或提前释放": "资源释放：重复释放或提前释放",
    "Kernel 包来源可疑": "触发：Kernel 包来源可疑",
    "需要从 Runtime 证据转向其他专项": "域外停止：证据转向 Precision 或 AI Core",
}
NEGATIVE_TRIGGER_TITLES = (
    "反向触发：编译错误不进入 Runtime",
    "反向触发：多卡通信等待不进入 Runtime",
)


def test_description_triggers_have_positive_and_negative_evals():
    skill = SKILL_PATH.read_text(encoding="utf-8")
    assert "触发：" in skill.split("\n---\n", 1)[0]

    document = json.loads(EVALS_PATH.read_text(encoding="utf-8"))
    evals = document["evals"]
    titles = {item["title"] for item in evals}
    assert len(titles) == len(evals)
    assert [item["id"] for item in evals] == list(range(1, len(evals) + 1))

    for trigger, title in POSITIVE_TRIGGER_TITLES.items():
        assert trigger in skill
        assert title in titles

    for title in NEGATIVE_TRIGGER_TITLES:
        assert title in titles
