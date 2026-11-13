# Copyright (c) 2026 AProf contributors.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
"""读取已通过冻结知识清单核验的正文引用；不升级历史知识的含义。"""

from pathlib import Path
import re

from .errors import GraphError
from .jsonio import file_hash
from .knowledge import local


def knowledge_source(root, ref):
    """调用者必须先确认 ref 属于声明的冻结 owner/record。"""
    root = Path(root)
    path = local(root, ref["path"])
    expected = ref["sha256"]
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", expected):
        raise GraphError("知识引用 hash 格式错误")
    if path.is_file() and file_hash(path) == expected:
        return path
    archived = local(root, f"workflows/references/knowledge-history/{expected[7:]}.md")
    if archived.is_file() and file_hash(archived) == expected:
        return archived
    raise GraphError(f"冻结知识正文缺失或 hash 不匹配: {ref['path']}")
