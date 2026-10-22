#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Regenerate references/interface.json from asc-tools msobjdump sources.

提取内容全部是 skill 依赖的接口面（参数、字段映射、跟踪文案），不做整文件哈希，
因此无关重构不会误报。curated 部分（选项分类、跟踪文案清单）在本文件维护：
新增 add_argument 选项或缺分类时生成失败，强制评审者补齐——这是刻意的守护点。
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

SCHEMA_VERSION = 1

# 选项语义分类（curated）：kind=primary 主操作，modifier 修饰项；
# optional_capability=True 表示较新版本才引入、旧安装缺失不算故障。
OPTION_CLASS: Dict[str, Dict[str, Any]] = {
    "--dump-elf": {"kind": "primary", "takes_value": True},
    "--verbose": {"kind": "modifier", "takes_value": False},
    "--extract-elf": {"kind": "primary", "takes_value": True},
    "--list-elf": {"kind": "primary", "takes_value": True},
    "--sass": {
        "kind": "primary",
        "takes_value": True,
        "optional_capability": True,
    },
    "--out-dir": {"kind": "modifier", "takes_value": True},
}

# skill 文档解释过的工具输出/报错文案；源码中消失或改写时生成失败。
TRACKED_MESSAGES = [
    "The kernel meta information cannot be found.",
    "nothing to list in single op elf file",
    "nothing to extra in single op elf file",
    "llvm-objcopy is not available",
    "llvm-objdump is not available",
    "no device ELF found in input",
    "thin archive is not supported",
    "llvm-objdump produced no valid instruction lines",
    "llvm-objdump produced no usable device instructions",
    "llvm-objdump output contains unknown instructions",
    "cannot be combined with",
]

COMMANDS = {
    "primary": "npu-objdump",
    "compat": "msobjdump",
    "module": "python3 -m msobjdump",
    "note": (
        "安装包主命令为 npu-objdump，msobjdump 为兼容软链，参数与行为一致；"
        "旧安装（如 CANN 9.2.0）只有 msobjdump。"
    ),
}

EXIT_CODES = {
    "0": "成功；仍需结合 stdout/stderr 与实际产物判断",
    "1": "运行错误（新版起向上传播）",
    "2": "参数错误（argparse：互斥组合、输入不存在等）",
}

MAIN_PY = "utils/msobjdump/msobjdump/msobjdump_main.py"

ARGUMENT_CALL = re.compile(r"add_argument\(\s*\"(--[a-z-]+)\"([^)]*)\)", re.DOTALL)
SHORT_OPTION = re.compile(r"\"(-[A-Za-z])\"")


def skill_dir() -> Path:
    return Path(__file__).resolve().parents[1]


def infer_source_root() -> Optional[Path]:
    candidates: List[Path] = []
    from os import environ

    configured = environ.get("ASC_TOOLS_ROOT")
    if configured:
        candidates.append(Path(configured).expanduser())
    candidates.append(Path.cwd())
    candidates.extend(skill_dir().parents)
    for candidate in candidates:
        if (candidate / MAIN_PY).is_file():
            return candidate.resolve()
    return None


def extract_options(source_text: str) -> List[Dict[str, Any]]:
    options: List[Dict[str, Any]] = []
    for match in ARGUMENT_CALL.finditer(source_text):
        name = match.group(1)
        rest = match.group(2)
        classified = OPTION_CLASS.get(name)
        if classified is None:
            raise SystemExit(
                "[ERROR]: option %s is defined in %s but has no classification "
                "in gen_interface.py OPTION_CLASS; classify it and rerun so the "
                "skill snapshot stays intentional." % (name, MAIN_PY)
            )
        option = {"names": [name]}
        shorts = SHORT_OPTION.findall(rest)
        option["names"] = [shorts[0], name] if shorts else [name]
        option["kind"] = classified["kind"]
        option["takes_value"] = (
            False if "store_true" in rest else classified["takes_value"]
        )
        if classified.get("optional_capability"):
            option["optional_capability"] = True
        options.append(option)
    if not options:
        raise SystemExit("[ERROR]: no add_argument options found in %s" % MAIN_PY)
    return options


def option_names(interface: Dict[str, Any]) -> set:
    return {
        name for option in documented_options(interface) for name in option["names"]
    }


def documented_options(interface: Dict[str, Any]) -> List[Dict[str, Any]]:
    return interface.get("cli", {}).get("options", [])


def extract_maps(source_root: Path) -> Dict[str, Dict[str, str]]:
    sys.path.insert(0, str(source_root / "utils/msobjdump"))
    try:
        import msobjdump.msobjdump_main as main_module  # noqa: C0415
    finally:
        sys.path.pop(0)
    return {
        "binary_meta": {str(k): v for k, v in main_module.B_TYPE_MAP.items()},
        "kernel_meta": {str(k): v for k, v in main_module.F_TYPE_MAP.items()},
        "kernel_type": dict(main_module.K_TYPE_MAP),
        "cross_core_sync": dict(main_module.C_TYPE_MAP),
        "runtime_implicit_info": {
            str(k): v for k, v in main_module.RUNTIME_IMPLICIT_INFO_MAP.items()
        },
    }


def check_messages(source_text: str, strict: bool = True) -> List[str]:
    """strict（重新生成）时缺消息即失败；--check 模式交由方向判定处理。"""
    found = [m for m in TRACKED_MESSAGES if m in source_text]
    missing = [m for m in TRACKED_MESSAGES if m not in found]
    if missing and strict:
        raise SystemExit(
            "[ERROR]: tracked messages no longer present in %s: %s\n"
            "Update TRACKED_MESSAGES in gen_interface.py and the skill "
            "references after reviewing the change."
            % (MAIN_PY, "; ".join(repr(m) for m in missing))
        )
    return found


def git_commit(source_root: Path) -> Optional[str]:
    try:
        result = subprocess.run(
            ["git", "-C", str(source_root), "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def build_interface(source_root: Path, strict: bool = True) -> Dict[str, Any]:
    main_py = source_root / MAIN_PY
    source_text = main_py.read_text(encoding="utf-8")
    return {
        "schema_version": SCHEMA_VERSION,
        "tool": "msobjdump",
        "provenance": {
            "asc_tools_commit": git_commit(source_root),
            "generated_by": "skills/tool-npu-objdump/scripts/gen_interface.py",
            "note": (
                "接口面语义快照（参数、字段映射、跟踪文案）。"
                "变更需重新生成并随 PR 评审；不做整文件哈希，无关重构不触发。"
            ),
        },
        "commands": COMMANDS,
        "cli": {
            "prog": "msobjdump",
            "options": extract_options(source_text),
            "exit_codes": EXIT_CODES,
        },
        "fields": extract_maps(source_root),
        "tracked_messages": check_messages(source_text, strict=strict),
    }


def render(interface: Dict[str, Any]) -> str:
    return json.dumps(interface, ensure_ascii=False, indent=2) + "\n"


def semantic_payload(interface: Dict[str, Any]) -> str:
    """provenance 不参与漂移比较：记录来源 commit，但 HEAD 前移不算接口变化。"""
    payload = {k: v for k, v in interface.items() if k != "provenance"}
    return json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Regenerate the tool-npu-objdump interface snapshot."
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        help="asc-tools source root; defaults to ASC_TOOLS_ROOT, cwd, or parents",
    )
    default_output = skill_dir() / "references/interface.json"
    parser.add_argument(
        "--output",
        type=Path,
        default=default_output,
        help="interface.json path (default: %(default)s)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="compare with the existing snapshot and exit non-zero on drift",
    )
    args = parser.parse_args()

    source_root = (
        args.source_root.expanduser().resolve()
        if args.source_root
        else infer_source_root()
    )
    if source_root is None:
        print(
            "[ERROR]: asc-tools source root not found; pass --source-root or set "
            "ASC_TOOLS_ROOT",
            file=sys.stderr,
        )
        return 1

    interface = build_interface(source_root, strict=not args.check)
    rendered = render(interface)

    if args.check:
        if not args.output.is_file():
            print(
                "[ERROR]: snapshot missing: %s\nRegenerate with gen_interface.py "
                "(without --check) and commit it with the interface change."
                % args.output,
                file=sys.stderr,
            )
            return 1
        try:
            stored = json.loads(args.output.read_text(encoding="utf-8"))
        except ValueError as error:
            print(
                "[ERROR]: snapshot %s is not valid JSON: %s" % (args.output, error),
                file=sys.stderr,
            )
            return 1
        stored_names = option_names(stored)
        current_names = option_names(interface)
        if stored_names == current_names:
            if semantic_payload(stored) == semantic_payload(interface):
                print("interface snapshot: in sync (%s)" % source_root)
                return 0
        elif stored_names > current_names:
            # 本 checkout 的 msobjdump 早于快照记载的接口（如 skill PR 基于
            # 旧 master 而接口改动在另一 MR）。这不是快照过期，守护不适用。
            gained = sorted(stored_names - current_names)
            print(
                "interface snapshot: guard inactive (%s predates the snapshot "
                "interface: %s missing here); guard engages once the interface "
                "change merges" % (source_root, ", ".join(gained))
            )
            return 0
        print(
            "[ERROR]: msobjdump interface changed but the skill snapshot is stale.\n"
            "Regenerate and review:\n"
            "  python3 skills/tool-npu-objdump/scripts/gen_interface.py \\\n"
            "    --source-root <asc-tools root>\n"
            "Then review SKILL.md / references / evals for semantic changes "
            "(new options need classification in OPTION_CLASS) and commit together.",
            file=sys.stderr,
        )
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    print("written: %s (from %s)" % (args.output, source_root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
