# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Protect directory delivery, mode validation and cleanup through the real CLI/IPC."""

import concurrent.futures
import json
from pathlib import Path
import re
import sqlite3
import subprocess

import pytest

from test_rep_packaging_integration import (
    CLI,
    FIXTURE_APP,
    HARDWARE_INFO,
    PIPE_CSV,
    SUMMARY,
    extract_path,
)


def collect(work, parent=None, sections=("PipeUtilization",), extra=()):
    work.mkdir(parents=True, exist_ok=True)
    command = [str(CLI), "--no-pack"]
    for section in sections:
        command += ["--section", section]
    if parent is not None:
        command += ["--export", str(parent)]
    command += [str(FIXTURE_APP), *extra]
    return subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=20)


@pytest.mark.parametrize("relative", [False, True])
def test_no_pack_keeps_raw_results_in_export_parent_without_package(tmp_path, relative):
    work = tmp_path / "work"
    parent = tmp_path / "results"
    parent.mkdir()
    sentinel = parent / "keep.txt"
    sentinel.write_text("existing user data")
    result = collect(work, Path("../results") if relative else parent)
    assert result.returncode == 0, result.stderr
    output = extract_path(result.stderr, "directory")
    assert output.is_absolute() and output.is_dir()
    assert output.parent == parent
    assert re.fullmatch(r"report_[0-9]+_[0-9a-f]{8}", output.name)
    assert sorted(p.name for p in output.iterdir()) == [
        "HardwareInfo.jsonl",
        "PipeUtilization.csv",
        "summary.db",
        "summary.jsonl",
    ]
    assert (output / "HardwareInfo.jsonl").read_bytes() == HARDWARE_INFO
    assert (output / "PipeUtilization.csv").read_bytes() == PIPE_CSV
    assert (output / "summary.jsonl").read_bytes() == SUMMARY
    assert not list(tmp_path.rglob("*.npu-rep"))
    assert not list(work.iterdir())
    assert sentinel.read_text() == "existing user data"
    assert "npu-compute: report=" not in result.stderr


def test_no_pack_defaults_to_current_directory(tmp_path):
    result = collect(tmp_path)
    assert result.returncode == 0, result.stderr
    output = extract_path(result.stderr, "directory")
    assert list(tmp_path.iterdir()) == [output]
    assert (output / "PipeUtilization.csv").read_bytes() == PIPE_CSV


def test_no_pack_pipeline_is_finalized_and_app_arguments_are_forwarded(tmp_path):
    result = collect(
        tmp_path,
        sections=("Pipeline", "Memory", "L2Cache"),
        extra=("--mode", "cli-echo", "--", "--no-pack", "--export", "app-value"),
    )
    assert result.returncode == 0, result.stderr
    output = extract_path(result.stderr, "directory")
    trace = json.loads((output / "PipeTrace.json").read_text())
    assert len(trace["traceEvents"]) == 1
    assert (output / "Memory.csv").read_bytes() == b"block_id,read_bytes\n0,128\n"
    assert (output / "L2Cache.csv").read_bytes() == b"block_id,hit_rate\n0,99\n"
    assert not (output / ".biu-staging").exists()
    assert "argument=2d2d6e6f2d7061636b" in result.stdout
    assert "argument=2d2d6578706f7274" in result.stdout


@pytest.mark.parametrize(
    "options",
    [
        ("--import", "missing.npu-rep"),
        ("--list-sections",),
        ("--list-sets",),
        ("--no-pack",),
    ],
)
def test_no_pack_invalid_modes_are_rejected(tmp_path, options):
    result = subprocess.run(
        [str(CLI), "--no-pack", *options],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2
    assert "unknown option '--no-pack'" not in result.stderr
    assert "directory=" not in result.stderr
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize(
    "kind", ["new-rep", "file", "missing", "symlink", "unwritable"]
)
def test_no_pack_invalid_export_does_not_start_app(tmp_path, kind):
    target = tmp_path / "target"
    if kind == "new-rep":
        target = tmp_path / "custom.npu-rep"
    elif kind == "file":
        target.write_text("keep")
    elif kind == "symlink":
        target.symlink_to(tmp_path, target_is_directory=True)
    elif kind == "unwritable":
        target = Path("/proc")
    command = [
        str(CLI),
        "--section",
        "Memory",
        "--no-pack",
        "--export",
        str(target),
        "/bin/sh",
        "-c",
        "touch app-ran",
    ]
    result = subprocess.run(
        command, cwd=tmp_path, capture_output=True, text=True, timeout=10
    )
    assert result.returncode == 4, result.stderr
    assert not (tmp_path / "app-ran").exists()
    assert "directory=" not in result.stderr
    if kind == "file":
        assert target.read_text() == "keep"


@pytest.mark.parametrize(
    "extra",
    [
        ("--exit-code", "23"),
        ("--mode", "empty"),
        ("--mode", "protocol-bad-offset"),
    ],
)
def test_no_pack_failed_collection_cleans_only_its_own_directory(tmp_path, extra):
    parent = tmp_path / "results"
    parent.mkdir()
    sentinel = parent / "keep"
    sentinel.write_text("keep")
    result = collect(tmp_path / "work", parent, extra=extra)
    assert result.returncode == (23 if extra[0] == "--exit-code" else 3), result.stderr
    assert "directory=" not in result.stderr
    assert list(parent.iterdir()) == [sentinel]
    assert sentinel.read_text() == "keep"


def test_no_pack_missing_pipeline_artifact_is_not_success(tmp_path):
    result = collect(tmp_path, sections=("Pipeline", "Memory"))
    assert result.returncode == 3, result.stderr
    assert "required Pipeline artifact missing" in result.stderr
    output = extract_path(result.stderr, "directory")
    assert (output / "Memory.csv").is_file()
    assert (output / "summary.db").is_file()
    assert not (output / "PipeTrace.json").exists()


def test_no_pack_concurrent_collections_have_distinct_complete_directories(tmp_path):
    parent = tmp_path / "results"
    parent.mkdir()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        runs = list(
            pool.map(lambda i: collect(tmp_path / f"work-{i}", parent), range(4))
        )
    assert all(r.returncode == 0 for r in runs), [r.stderr for r in runs]
    outputs = {extract_path(r.stderr, "directory") for r in runs}
    assert len(outputs) == 4
    assert set(parent.iterdir()) == outputs
    for output in outputs:
        assert (output / "PipeUtilization.csv").read_bytes() == PIPE_CSV


def test_no_pack_help_uses_new_option_name():
    result = subprocess.run(
        [str(CLI), "--help"], capture_output=True, text=True, timeout=10
    )
    assert result.returncode == 0
    assert "--no-pack" in result.stdout
    assert "--disk" not in result.stdout


def test_no_pack_rejects_old_disk_option_before_starting_app(tmp_path):
    result = subprocess.run(
        [str(CLI), "--disk", "/bin/sh", "-c", "touch app-ran"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2
    assert "unknown option '--disk'" in result.stderr
    assert not (tmp_path / "app-ran").exists()
    assert list(tmp_path.iterdir()) == []


def test_no_pack_can_follow_export_before_app(tmp_path):
    parent = tmp_path / "results"
    parent.mkdir()
    result = subprocess.run(
        [
            str(CLI),
            "--export",
            str(parent),
            "--no-pack",
            "--section",
            "PipeUtilization",
            str(FIXTURE_APP),
        ],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert result.returncode == 0, result.stderr
    output = extract_path(result.stderr, "directory")
    assert output.parent == parent
    assert (output / "PipeUtilization.csv").read_bytes() == PIPE_CSV
    assert not list(tmp_path.rglob("*.npu-rep"))


@pytest.mark.parametrize("mode", ["incomplete", "missing-hardware", "invalid-csv"])
def test_no_pack_partial_results_survive(tmp_path, mode):
    result = collect(tmp_path, extra=("--mode", mode))
    assert result.returncode == 3, result.stderr
    output = extract_path(result.stderr, "directory")
    assert (output / "summary.jsonl").read_bytes() == SUMMARY
    assert (output / "PipeUtilization.csv").exists() == (mode != "invalid-csv")
    assert (output / "HardwareInfo.jsonl").exists() == (mode != "missing-hardware")
    assert not list(output.rglob("*.tmp*"))
    assert "valid raw results were retained" in result.stderr


@pytest.mark.parametrize(
    "mode", ["success", "csv-field-count", "csv-unit-conflict", "csv-directories"]
)
def test_no_pack_csv_conversion_preserves_raw_sources(tmp_path, mode):
    result = collect(
        tmp_path, sections=("Memory", "PipeUtilization"), extra=("--mode", mode)
    )
    assert result.returncode == (
        3 if mode in ("csv-field-count", "csv-unit-conflict") else 0
    ), result.stderr
    output = extract_path(result.stderr, "directory")
    assert (output / "Memory.csv").is_file()
    assert (output / "PipeUtilization.csv").is_file()
    with sqlite3.connect(output / "summary.db") as database:
        tables = {
            row[0]
            for row in database.execute(
                "SELECT name FROM sqlite_master WHERE type='table'"
            )
        }
        expected = {"Memory", "PipeUtilization", "FieldUnitMap"}
        if mode == "csv-field-count":
            expected.remove("Memory")
            assert (output / "Memory.csv").read_bytes() == b"block_id,read_bytes\n0\n"
        elif mode == "csv-unit-conflict":
            expected.remove("PipeUtilization")
        assert tables == expected
    if mode == "csv-directories":
        with sqlite3.connect(output / "collection-p1-0001" / "summary.db") as database:
            assert database.execute("SELECT * FROM Memory").fetchall() == [(1, 256)]
    assert not list(output.rglob("*.tmp*"))


@pytest.mark.parametrize(
    "sections",
    [("PmSampling",), ("PmSampling", "Memory"), ("PmSampling", "Pipeline", "Memory")],
)
@pytest.mark.parametrize("export_style", ["default", "before", "after", "equals"])
def test_no_pack_pm_sampling_option_compatibility(tmp_path, sections, export_style):
    parent = tmp_path / "results"
    parent.mkdir()
    options = ["--no-pack"]
    if export_style == "before":
        options = ["--export", str(parent), *options]
    elif export_style == "after":
        options += ["--export", str(parent)]
    elif export_style == "equals":
        options += [f"--export={parent}"]
    for section in sections:
        options += ["--section", section]
    result = subprocess.run(
        [str(CLI), *options, str(FIXTURE_APP), "--mode", "pm-sampling"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stderr
    output = extract_path(result.stderr, "directory")
    assert output.parent == (tmp_path if export_style == "default" else parent)
    with sqlite3.connect(output / "summary.db") as database:
        assert database.execute("SELECT address FROM PcSampling").fetchall() == [
            (0x268,)
        ]
        if "Memory" in sections:
            assert database.execute("SELECT * FROM Memory").fetchall() == [(0, 128)]
    assert "268:" in (output / "objdump.txt").read_text()
    assert len(json.loads((output / "symbolizer.json").read_text())) == 1
    if "Pipeline" in sections:
        assert (
            len(json.loads((output / "PipeTrace.json").read_text())["traceEvents"]) == 1
        )
    assert not list(output.rglob(".biu-staging"))
    assert not list(tmp_path.rglob("*.npu-rep"))


@pytest.mark.parametrize("artifact", ["db", "objdump", "symbolizer"])
@pytest.mark.parametrize("failure", ["missing", "invalid"])
def test_no_pack_sampling_failure_preserves_other_artifacts(
    tmp_path, artifact, failure
):
    result = collect(
        tmp_path,
        sections=("PmSampling",),
        extra=("--mode", f"pm-sampling-{failure}-{artifact}"),
    )
    assert result.returncode == 3, result.stderr
    output = extract_path(result.stderr, "directory")
    names = {
        "db": "summary.db",
        "objdump": "objdump.txt",
        "symbolizer": "symbolizer.json",
    }
    assert not (output / names[artifact]).exists()
    assert (output / "HardwareInfo.jsonl").read_bytes() == HARDWARE_INFO
    for kind, name in names.items():
        if kind != artifact:
            assert (output / name).is_file()
    if artifact != "db":
        with sqlite3.connect(output / "summary.db") as database:
            assert database.execute("SELECT address FROM PcSampling").fetchall() == [
                (0x268,)
            ]


def test_no_pack_failed_csv_conversion_preserves_sampling_database(tmp_path):
    result = collect(
        tmp_path,
        sections=("PmSampling", "Memory"),
        extra=("--mode", "pm-sampling-csv-field-count"),
    )
    assert result.returncode == 3, result.stderr
    output = extract_path(result.stderr, "directory")
    with sqlite3.connect(output / "summary.db") as database:
        assert database.execute("SELECT address FROM PcSampling").fetchall() == [
            (0x268,)
        ]
        assert (
            database.execute(
                "SELECT name FROM sqlite_master WHERE name='Memory'"
            ).fetchall()
            == []
        )
    assert (output / "Memory.csv").read_bytes() == b"block_id,read_bytes\n0\n"
    assert (output / "objdump.txt").is_file()
    assert (output / "symbolizer.json").is_file()
    assert not list(output.rglob("*.tmp*"))


def test_no_pack_full_set_contains_sampling_and_csv_tables(tmp_path):
    result = subprocess.run(
        [
            str(CLI),
            "--set",
            "full",
            "--no-pack",
            str(FIXTURE_APP),
            "--mode",
            "pm-sampling",
        ],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stderr
    output = extract_path(result.stderr, "directory")
    with sqlite3.connect(output / "summary.db") as database:
        tables = {
            row[0]
            for row in database.execute(
                "SELECT name FROM sqlite_master WHERE type='table'"
            )
        }
        assert {
            "PcSampling",
            "Memory",
            "PipeUtilization",
            "ResourceConflictRatio",
        } <= tables
    assert (output / "objdump.txt").is_file()
    assert (output / "symbolizer.json").is_file()
    assert len(json.loads((output / "PipeTrace.json").read_text())["traceEvents"]) == 1


@pytest.mark.parametrize("value", ["true", "false"])
def test_no_pack_does_not_accept_a_value(tmp_path, value):
    result = subprocess.run(
        [str(CLI), f"--no-pack={value}", "/bin/sh", "-c", "touch app-ran"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 2
    assert not (tmp_path / "app-ran").exists()
