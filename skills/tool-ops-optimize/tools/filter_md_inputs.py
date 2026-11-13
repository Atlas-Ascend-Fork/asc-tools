#!/usr/bin/env python3
# Copyright (c) 2026 AProf contributors.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
"""Exclude verified historical Markdown snapshots from current-document checks."""

import argparse
import json
from pathlib import Path

from check_import import verify, _DOCS


def filter_filelist(root, filelist):
    root = Path(root)
    filelist = Path(filelist)
    # Never hide modified snapshots or newly added files from the document check.
    verify(root)
    manifest = json.loads(
        (root / "tools/frozen-files.json").read_text(encoding="utf-8")
    )
    adapted = _DOCS.adaptations(root)
    frozen = {
        "skills/tool-ops-optimize/" + name
        for name in manifest["files"]
        if name.endswith(".md") and name not in adapted
    }
    original = filelist.read_text(encoding="utf-8").splitlines(keepends=True)
    retained = [line for line in original if line.strip() not in frozen]
    filelist.write_text("".join(retained), encoding="utf-8", newline="\n")
    print(
        "MD inputs: excluded {} verified snapshots; retained {} entries.".format(
            len(original) - len(retained), len(retained)
        )
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("filelist", type=Path)
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    args = parser.parse_args()
    filter_filelist(args.root, args.filelist)
