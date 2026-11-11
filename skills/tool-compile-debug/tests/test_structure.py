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
"""包结构约束测试。

用途：钉住技能目录结构、frontmatter 与脚本头部约定，防止包结构漂移。
使用方法：python3 -m unittest discover -s tests  （在技能根目录执行）
"""

import json
import re
import tempfile
import unittest
from pathlib import Path

SKILL_ROOT = Path(__file__).resolve().parents[1]
SKILL_MD = SKILL_ROOT / "SKILL.md"
LICENSE_MARK = "CANN Open Software License Agreement Version 2.0"
EXTERNAL_SKILL_NAME = (
    r"(?:ascendc-[a-z0-9]+(?:-[a-z0-9]+)*|npu-arch|ops-precision-standard)"
)
# Match skill names in prose, including names without backticks, but not options
# such as --npu-arch or path components such as /ascendc-compile-doctor/audit.
EXTERNAL_SKILL_RE = re.compile(
    rf"(?<![A-Za-z0-9_./-]){EXTERNAL_SKILL_NAME}(?![A-Za-z0-9_./-])"
)
QUALIFIED_SKILL_RE = re.compile(
    rf"(?<![A-Za-z0-9_-])CANNBOT\s+`({EXTERNAL_SKILL_NAME})`\s+技能"
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
    # Fenced examples contain exact machine identifiers, not prose references.
    # Preserve offsets/newlines so errors still point at the original document.
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

    def test_description_declares_trigger(self):
        description = self.frontmatter.get("description", "")
        self.assertTrue(description)
        self.assertLessEqual(len(description), 1024)
        self.assertIn("触发：", description)
        # 折叠块原文里也必须显式出现「触发：」（触发约束的字面要求）
        self.assertIn("触发：", self.frontmatter_block)

    def test_skill_is_concise(self):
        self.assertLess(len(self.text.splitlines()), 500)

    def test_external_skill_references_identify_provider_and_role(self):
        for path in package_files(SKILL_ROOT):
            if path.suffix != ".md":
                continue
            with self.subTest(file=path.relative_to(SKILL_ROOT).as_posix()):
                self.assertEqual(
                    unqualified_skill_references(path.read_text(encoding="utf-8")),
                    [],
                    "外部技能应写为 CANNBOT `技能名` 技能",
                )
        for path in sorted((SKILL_ROOT / "evals").glob("*.json")):
            document = json.loads(path.read_text(encoding="utf-8"))
            for location, text in evaluation_prose(document):
                with self.subTest(file=path.name, field=location):
                    self.assertEqual(unqualified_skill_references(text), [])

    def test_external_skill_reference_rule_rejects_unqualified_prose(self):
        for text in (
            "调用 `ascendc-docs-search` 技能",
            "架构事实走 npu-arch 技能",
            "调用 CANNBOT ascendc-docs-search 技能",
            "调用 `CANNBOT ascendc-docs-search` 技能",
            "调用 CANNBOT `ascendc-docs-search`",
            "调用 NOTCANNBOT `ascendc-docs-search` 技能",
            "调用 CANNBOT `ascendc-docs-search` 或 `npu-arch` 技能",
        ):
            with self.subTest(text=text):
                self.assertTrue(unqualified_skill_references(text))
        valid = (
            "调用 CANNBOT `ascendc-docs-search` 技能；"
            "CANNBOT `npu-arch` 技能核对 --npu-arch 的值。\n"
            "CANNBOT `ops-precision-standard`\n技能用于验收；"
            "日志位于 /state/ascendc-compile-doctor/audit。"
        )
        self.assertEqual(unqualified_skill_references(valid), [])

    def test_fenced_machine_names_do_not_relax_following_prose(self):
        for fence in ("```", "~~~~"):
            example = fence + 'json\n{"skill":"ascendc-docs-search"}\n' + fence + "\n"
            self.assertEqual(unqualified_skill_references(example), [])
            self.assertEqual(
                unqualified_skill_references(example + "调用 `npu-arch` 技能"),
                ["line 4: npu-arch"],
            )

    def test_evaluation_prose_checks_descriptions_but_not_machine_identifiers(self):
        document = {
            "skill_name": "ascendc-env-check",
            "config": {"distractor_skills": ["ascendc-env-check"]},
            "prompt": "核对 --npu-arch 参数。",
            "expectations": [
                {
                    "type": "contains",
                    "pattern": "npu-arch",
                    "description": "架构事实走 npu-arch 技能",
                }
            ],
        }
        fields = dict(evaluation_prose(document))
        self.assertEqual(set(fields), {"/prompt", "/expectations/0/description"})
        self.assertEqual(unqualified_skill_references(fields["/prompt"]), [])
        self.assertTrue(
            unqualified_skill_references(fields["/expectations/0/description"])
        )

    def test_package_has_no_empty_or_whitespace_only_files(self):
        files = [
            path
            for path in package_files(SKILL_ROOT)
            if "__pycache__" not in path.parts
        ]
        self.assertTrue(files)
        empty = [
            path.relative_to(SKILL_ROOT).as_posix()
            for path in files
            if not path.read_bytes().strip()
        ]
        self.assertEqual(empty, [], f"Empty package files: {empty}")

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
        # 宿主仓技能根惯例：不携带 README/requirements/CHANGELOG/.gitattributes/.ps1
        for name in (
            "README.md",
            "requirements.txt",
            "CHANGELOG.md",
            ".gitattributes",
        ):
            self.assertFalse((SKILL_ROOT / name).is_file(), f"技能根不应有 {name}")
        self.assertFalse(
            [path for path in package_files(SKILL_ROOT) if path.suffix == ".ps1"]
        )

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

    def test_scripts_carry_license_header_and_purpose(self):
        scripts = sorted((SKILL_ROOT / "scripts").glob("*.py"))
        scripts += sorted((SKILL_ROOT / "scripts").glob("*.sh"))
        scripts.append(
            SKILL_ROOT / "scripts" / "engine" / "harness/runtime" / "build_one.sh"
        )
        scripts.append(
            SKILL_ROOT
            / "scripts"
            / "engine"
            / "harness"
            / "engine"
            / "probe_msopgen_regenerate_poc.py"
        )
        self.assertTrue(scripts)
        for script in scripts:
            head = "\n".join(script.read_text(encoding="utf-8").splitlines()[:60])
            self.assertIn(LICENSE_MARK, head, f"{script.name} 缺 CANN-2.0 许可头")
            self.assertIn("用途", head, f"{script.name} 缺用途说明")


if __name__ == "__main__":
    unittest.main()
