# Copyright (c) 2026 AProf contributors.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
"""The CI exemptions must reject altered or newly added frozen payloads."""

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest
import test_source_archives as archive_tests

archived = archive_tests.archived
NAME = archive_tests.NAME

ENTRY = Path(__file__).resolve().parents[2] / "tools/check_import.py"
SPEC = importlib.util.spec_from_file_location("check_import", ENTRY)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


@pytest.fixture
def package(tmp_path):
    (tmp_path / "sources").mkdir()
    source = tmp_path / "sources/original.txt"
    source.write_bytes(b"original\r\n")
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools/frozen-files.json").write_text(
        json.dumps(
            {
                "files": {
                    "sources/original.txt": hashlib.sha256(
                        source.read_bytes()
                    ).hexdigest()
                }
            }
        )
    )
    (tmp_path / "skills/core").mkdir(parents=True)
    (tmp_path / "skills/core/SKILL.md").write_text("---\nname: core\n---\n")
    (tmp_path / "skills/registry.json").write_text(
        json.dumps(
            {
                "capabilities": {"core": [{"name": "core", "source": "skills/core"}]},
                "support_skills": [],
                "workflow_plugin": {"agents": []},
            }
        )
    )
    return tmp_path


def test_valid_frozen_payload(package):
    CHECK.verify(package)


def test_line_ending_rewrite_is_rejected(package):
    (package / "sources/original.txt").write_bytes(b"original\n")
    with pytest.raises(ValueError, match="frozen bytes changed"):
        CHECK.verify(package)


def test_unregistered_frozen_file_is_rejected(package):
    (package / "sources/new.txt").write_bytes(b"unreviewed")
    with pytest.raises(ValueError, match="frozen inventory differs"):
        CHECK.verify(package)


def test_broken_skill_entry_is_rejected(package):
    (package / "skills/core/SKILL.md").write_text("---\nname: wrong\n---\n")
    with pytest.raises(ValueError, match="skill name differs"):
        CHECK.verify(package)


def test_markdown_filter_keeps_active_and_other_documents(package):
    history = package / "sources/history.md"
    history.write_bytes(b"# Historical snapshot\n")
    manifest_path = package / "tools/frozen-files.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["files"]["sources/history.md"] = hashlib.sha256(
        history.read_bytes()
    ).hexdigest()
    manifest_path.write_text(json.dumps(manifest))
    retained = (
        "skills/tool-ops-optimize/SKILL.md\n"
        "skills/another-skill/SKILL.md\n"
        "skills/tool-ops-optimize/sources/original.txt\n"
    )
    filelist = package / "pr_filelist.txt"
    filelist.write_text("skills/tool-ops-optimize/sources/history.md\n" + retained)
    subprocess.run(
        [
            sys.executable,
            str(ENTRY.with_name("filter_md_inputs.py")),
            str(filelist),
            "--root",
            str(package),
        ],
        check=True,
        capture_output=True,
    )
    assert filelist.read_text() == retained


def test_markdown_filter_rejects_changed_snapshot_before_filtering(package):
    (package / "sources/original.txt").write_bytes(b"changed")
    filelist = package / "pr_filelist.txt"
    original = "skills/tool-ops-optimize/SKILL.md\n"
    filelist.write_text(original)
    result = subprocess.run(
        [
            sys.executable,
            str(ENTRY.with_name("filter_md_inputs.py")),
            str(filelist),
            "--root",
            str(package),
        ],
        capture_output=True,
    )
    assert result.returncode != 0
    assert b"frozen bytes changed" in result.stderr
    assert filelist.read_text() == original


def test_archive_import_preserves_original_inventory(archived):
    root, _, _ = archived
    CHECK.verify(root)


def test_archive_corruption_is_rejected_by_import(archived):
    root, physical, _ = archived
    physical.write_bytes(b"corrupted")
    with pytest.raises(ValueError, match="archived source bytes changed"):
        CHECK.verify(root)


def test_archive_manifest_cannot_replace_frozen_source_identity(archived):
    root, physical, _ = archived
    physical.write_bytes(b"replacement")
    path = root / "tools/source-archives.json"
    manifest = json.loads(path.read_text())
    manifest["files"][NAME]["sha256"] = hashlib.sha256(
        physical.read_bytes()
    ).hexdigest()
    path.write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match="differs from frozen checksum"):
        CHECK.verify(root)


def test_unmapped_text_is_rejected_by_inventory(archived):
    root, _, _ = archived
    (root / "tools/source-archives.json").unlink()
    with pytest.raises(ValueError, match="frozen inventory differs"):
        CHECK.verify(root)


def test_shadow_header_is_rejected_by_inventory(archived):
    root, _, payload = archived
    (root / NAME).write_bytes(payload)
    with pytest.raises(ValueError, match="frozen inventory differs"):
        CHECK.verify(root)
