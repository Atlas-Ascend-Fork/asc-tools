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
"""包结构约束测试。

用途：钉住双层发行目录与内层纯净技能目录结构、frontmatter 与脚本头部约定，防止包结构漂移。
使用方法：python3 -m unittest discover -s tests  （在外层发行根目录执行）
"""

import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

import yaml

_TESTS_DIR = Path(__file__).resolve().parent
if str(_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(_TESTS_DIR))

from package_paths import resolve_roots  # noqa: E402

DIST_ROOT, SKILL_ROOT = resolve_roots(__file__)
SKILL_MD = SKILL_ROOT / "SKILL.md"
LICENSE_MARK = "CANN Open Software License Agreement Version 2.0"
EXTERNAL_SKILL_NAME = r"(?:knowledge-query|ascendc-api-knowledge-query)"
EXTERNAL_SKILL_RE = re.compile(
    rf"(?<![A-Za-z0-9_./-]){EXTERNAL_SKILL_NAME}(?![A-Za-z0-9_./-])"
)
QUALIFIED_SKILL_RE = re.compile(
    rf"(?<![A-Za-z0-9_-])cannbot-knowledge\s+`({EXTERNAL_SKILL_NAME})`\s+技能"
)
EVAL_PROSE_FIELDS = {
    "title",
    "prompt",
    "expected_output",
    "context",
    "reason",
    "description",
}


def package_files(root):
    """Yield payload files without entering repository metadata at the root."""
    for child in sorted(root.iterdir()):
        if child.name == ".git":
            continue
        if child.is_file():
            yield child
        elif child.is_dir():
            yield from (path for path in sorted(child.rglob("*")) if path.is_file())


def unqualified_skill_references(text):
    """Find prose references missing their adjacent provider, quoting or role."""
    fence = None
    lines = []
    for line in text.splitlines(keepends=True):
        opening = re.match(r"^ {0,3}(`{3,}|~{3,})", line)
        if fence is None and opening:
            fence = opening.group(1)
            lines.append(re.sub(r"[^\r\n]", " ", line))
        elif fence is not None:
            lines.append(re.sub(r"[^\r\n]", " ", line))
            if re.fullmatch(
                r" {0,3}"
                + re.escape(fence[0])
                + r"{"
                + str(len(fence))
                + r",}[ \t]*\r?\n?",
                line,
            ):
                fence = None
        else:
            lines.append(line)
    text = "".join(lines)
    qualified = {match.span(1) for match in QUALIFIED_SKILL_RE.finditer(text)}
    return [
        f"line {text.count(chr(10), 0, match.start()) + 1}: {match.group()}"
        for match in EXTERNAL_SKILL_RE.finditer(text)
        if match.span() not in qualified
    ]


def evaluation_prose(value, location=""):
    """Yield human-facing fields, leaving configuration and match patterns intact."""
    if isinstance(value, dict):
        for key, child in value.items():
            if key == "config":
                continue
            child_location = f"{location}/{key}"
            if key in EVAL_PROSE_FIELDS and isinstance(child, str):
                yield child_location, child
            elif isinstance(child, (dict, list)):
                yield from evaluation_prose(child, child_location)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from evaluation_prose(child, f"{location}/{index}")


def parse_frontmatter(text):
    """Return (字段字典, frontmatter 原文块)。折叠块标量（>-）的值以续行拼接。"""
    lines = text.splitlines()
    if not lines or lines[0] != "---":
        raise AssertionError("SKILL.md must start with YAML frontmatter")
    end = lines.index("---", 1)
    block = lines[1:end]
    fields: dict[str, str] = {}
    current = None
    for line in block:
        key, sep, value = line.partition(":")
        if sep and not line.startswith((" ", "\t")):
            current = key.strip()
            if value.strip() not in {">-", ">", "|", "|-"}:
                fields[current] = value.strip()
                current = None
        elif current:
            fields[current] = (fields.get(current, "") + " " + line.strip()).strip()
    return fields, "\n".join(block)


def unique_mapping(pairs):
    """Reject duplicate keys, including nested JSON/YAML configuration fields."""
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate configuration key: {key}")
        result[key] = value
    return result


class UniqueKeyLoader(yaml.SafeLoader):
    """Safe YAML loader with the duplicate-key gate used by upstream CI."""

    def construct_mapping(self, node, deep=False):
        self.flatten_mapping(node)
        return unique_mapping(
            (
                self.construct_object(key, deep=deep),
                self.construct_object(value, deep=deep),
            )
            for key, value in node.value
        )


class SkillStructureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = SKILL_MD.read_text(encoding="utf-8")
        cls.frontmatter, cls.frontmatter_block = parse_frontmatter(cls.text)

    def test_frontmatter_metadata(self):
        self.assertEqual(self.frontmatter.get("name"), "tool-compile-debug")
        self.assertRegex(self.frontmatter["name"], r"^[a-z0-9]+(-[a-z0-9]+)*$")
        self.assertLess(len(self.frontmatter["name"]), 64)
        self.assertEqual(self.frontmatter.get("license"), "CANN-2.0")

    def test_all_configuration_files_have_unique_keys(self):
        yaml.load(self.frontmatter_block, Loader=UniqueKeyLoader)
        scan_root = DIST_ROOT if (DIST_ROOT and DIST_ROOT.is_dir()) else SKILL_ROOT
        for path in package_files(scan_root):
            if path.suffix not in {".yaml", ".yml", ".json"}:
                continue
            with self.subTest(file=path.relative_to(scan_root).as_posix()):
                text = path.read_text(encoding="utf-8")
                if path.suffix == ".json":
                    json.loads(text, object_pairs_hook=unique_mapping)
                else:
                    list(yaml.load_all(text, Loader=UniqueKeyLoader))

    def test_duplicate_key_gate_rejects_ambiguous_configuration(self):
        for text in ("rules:\nrules: []\n", "outer:\n  key: 1\n  key: 2\n"):
            with (
                self.subTest(text=text),
                self.assertRaisesRegex(ValueError, "Duplicate"),
            ):
                yaml.load(text, Loader=UniqueKeyLoader)
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            json.loads('{"outer":{"key":1,"key":2}}', object_pairs_hook=unique_mapping)
        self.assertEqual(
            yaml.load("rules: []\n", Loader=UniqueKeyLoader), {"rules": []}
        )

    def test_compiler_flags_are_not_dependency_skill_names(self):
        for path in package_files(SKILL_ROOT):
            if path.suffix not in {".md", ".json"}:
                continue
            with self.subTest(file=path.relative_to(SKILL_ROOT).as_posix()):
                self.assertNotRegex(
                    path.read_text(encoding="utf-8"),
                    r"--(?:knowledge-query|ascendc-api-knowledge-query)\b",
                )

    def test_external_skill_references_identify_provider_and_role(self):
        docs = [SKILL_MD, *sorted((SKILL_ROOT / "references").rglob("*.md"))]
        if DIST_ROOT and (DIST_ROOT / "README.md").is_file():
            docs.append(DIST_ROOT / "README.md")
        for path in docs:
            rel = (
                path.relative_to(DIST_ROOT).as_posix()
                if (DIST_ROOT and path.is_relative_to(DIST_ROOT))
                else path.relative_to(SKILL_ROOT).as_posix()
            )
            with self.subTest(file=rel):
                self.assertEqual(
                    unqualified_skill_references(path.read_text(encoding="utf-8")),
                    [],
                )
        for path in sorted((SKILL_ROOT / "evals").glob("*.json")):
            rel = path.relative_to(SKILL_ROOT).as_posix()
            payload = json.loads(path.read_text(encoding="utf-8"))
            for loc, prose in evaluation_prose(payload):
                with self.subTest(file=rel, location=loc):
                    self.assertEqual(unqualified_skill_references(prose), [])

    def test_description_declares_trigger(self):
        description = self.frontmatter.get("description", "")
        self.assertTrue(description)
        self.assertLessEqual(len(description), 1024)
        self.assertIn("触发：", description)
        self.assertIn("触发：", self.frontmatter_block)

    def test_skill_is_concise(self):
        self.assertLess(len(self.text.splitlines()), 500)

    def test_package_has_no_empty_or_whitespace_only_files(self):
        roots = [SKILL_ROOT]
        if DIST_ROOT and DIST_ROOT.is_dir():
            roots.append(DIST_ROOT)
        for root in roots:
            files = [
                path
                for path in package_files(root)
                if "__pycache__" not in path.parts
                and ".pytest_cache" not in path.parts
                and path.suffix != ".pyc"
            ]
            self.assertTrue(files)
            empty = [
                path.relative_to(root).as_posix()
                for path in files
                if not path.read_bytes().strip()
            ]
            self.assertEqual(empty, [], f"Empty package files in {root}: {empty}")

    def test_package_files_exclude_only_root_git_metadata(self):
        for git_is_directory in (True, False):
            with self.subTest(git_is_directory=git_is_directory):
                with tempfile.TemporaryDirectory(prefix="skill-payload-") as raw:
                    root = Path(raw)
                    metadata = root / ".git"
                    if git_is_directory:
                        metadata.mkdir()
                        (metadata / "empty.md").write_text("", encoding="utf-8")
                    else:
                        metadata.write_text("", encoding="utf-8")
                    expected = {"SKILL.md", ".hidden.md", "docs/.git/fixture.md"}
                    for name in expected:
                        path = root / name
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text("", encoding="utf-8")
                    actual = {
                        path.relative_to(root).as_posix()
                        for path in package_files(root)
                    }
                    self.assertEqual(actual, expected)

    def test_root_has_no_host_repo_files(self):
        top_names = {
            p.name
            for p in SKILL_ROOT.iterdir()
            if p.name not in {".git", "__pycache__", ".pytest_cache"}
        }
        self.assertEqual(top_names, {"SKILL.md", "references", "evals", "scripts"})
        for name in (
            "README.md",
            "requirements.txt",
            "CHANGELOG.md",
            ".gitattributes",
            "tests",
            "scripts/install.sh",
            "scripts/install.py",
            "scripts/knowledge.lock.json",
        ):
            self.assertFalse(
                (SKILL_ROOT / name).exists(), f"内层纯净技能包不应有 {name}"
            )
        scan_root = DIST_ROOT if (DIST_ROOT and DIST_ROOT.is_dir()) else SKILL_ROOT
        self.assertFalse(
            [path for path in package_files(scan_root) if path.suffix == ".ps1"]
        )
        if DIST_ROOT and (DIST_ROOT / "scripts/install.sh").is_file():
            self.assertTrue((DIST_ROOT / "README.md").is_file())

    def test_local_links_resolve(self):
        links = re.findall(r"\[[^\]]+\]\(([^)#]+?)(?:#[^)]*)?\)", self.text)
        local = [
            link
            for link in links
            if "://" not in link and not link.startswith("skill:")
        ]
        self.assertTrue(local)
        for link in local:
            self.assertTrue((SKILL_ROOT / link).is_file(), link)
        if DIST_ROOT and (DIST_ROOT / "README.md").is_file():
            readme_text = (DIST_ROOT / "README.md").read_text(encoding="utf-8")
            outer_links = [
                link
                for link in re.findall(
                    r"\[[^\]]+\]\(([^)#]+?)(?:#[^)]*)?\)", readme_text
                )
                if "://" not in link and not link.startswith("skill:")
            ]
            self.assertTrue(outer_links)
            for link in outer_links:
                self.assertTrue((DIST_ROOT / link).is_file(), link)

    def test_all_python_and_shell_files_carry_copyright(self):
        scan_root = DIST_ROOT if (DIST_ROOT and DIST_ROOT.is_dir()) else SKILL_ROOT
        scripts = [
            path
            for path in package_files(scan_root)
            if path.suffix in {".py", ".sh"}
            and "__pycache__" not in path.parts
            and ".pytest_cache" not in path.parts
        ]
        self.assertTrue(scripts)
        for script in scripts:
            with self.subTest(script=script.relative_to(scan_root).as_posix()):
                head = "\n".join(script.read_text(encoding="utf-8").splitlines()[:20])
                self.assertIn(LICENSE_MARK, head)
                self.assertRegex(
                    head,
                    r"(?m)^# Copyright \(c\) \d{4}(?:-\d{4})? Huawei Technologies Co\., Ltd\.$",
                )

    def test_scripts_carry_license_header_and_purpose(self):
        scripts = sorted((SKILL_ROOT / "scripts").glob("*.py"))
        scripts += sorted((SKILL_ROOT / "scripts").glob("*.sh"))
        scripts.append(
            SKILL_ROOT / "scripts" / "engine" / "harness/runtime" / "build_one.sh"
        )
        if DIST_ROOT and (DIST_ROOT / "scripts" / "install.sh").is_file():
            scripts.append(DIST_ROOT / "scripts" / "install.sh")
        self.assertTrue(scripts)
        for script in scripts:
            head = "\n".join(script.read_text(encoding="utf-8").splitlines()[:60])
            self.assertIn(LICENSE_MARK, head, f"{script.name} 缺 CANN-2.0 许可头")
            self.assertIn("用途", head, f"{script.name} 缺用途说明")

    def test_outer_readme_title_and_sdk_prerequisite_guidance(self):
        self.assertIn("# Ascend C 编译问题定位与修复", self.text)
        if DIST_ROOT and (DIST_ROOT / "README.md").is_file():
            readme = (DIST_ROOT / "README.md").read_text(encoding="utf-8")
            self.assertTrue(
                readme.startswith("# Ascend C 编译问题定位与修复 Skill\n"),
                "外层 README 标题需与问题定位与修复功能一致",
            )
            self.assertIn("不是必需的", readme)
            self.assertIn("set_env.sh", readme)


if __name__ == "__main__":
    unittest.main()
