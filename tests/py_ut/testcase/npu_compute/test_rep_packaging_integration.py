# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import os
import json
import re
import struct
import sqlite3
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

import pytest


CLI = Path(os.environ["NPU_COMPUTE_TEST_CLI"])
FIXTURE_APP = Path(os.environ["NPU_COMPUTE_REP_FIXTURE_APP"])

MAGIC = b"npu-rep\0"
VERSION = 0x00010000
ORIGIN = 1
HEAD = struct.Struct("<8sIHHIIIQ")
FILE_INFO = struct.Struct("<8s128sHHIQQ")
TYPE_NPU_REP = 1
TYPE_JSONL = 3
TYPE_CSV = 4
TYPE_SQLITE = 5

HARDWARE_INFO = (
    b'{"category":"Host Info","cpu physical count":1}\n'
    b'{"category":"Device Info","npu count":1}\n'
    b'{"category":"CPU Information","control cpu count":1}\n'
    b'{"category":"AI Core Information","ai core count":1}\n'
    b'{"category":"Memory Information","hbm total(MB)":1}\n'
)
PIPE_CSV = b"block_id,pipe_utilization\n0,75\n"
SUMMARY = b'{"category":"PipeUtilization"}\n{"category":"OpInfoSummary"}\n'


@dataclass
class RepEntry:
    name: str
    file_type: int
    payload: bytes
    offset: int
    child: Optional["DecodedRep"]


@dataclass
class DecodedRep:
    entries: List[RepEntry]


def decode_rep(data: bytes) -> DecodedRep:
    assert len(data) >= HEAD.size
    magic, version, origin, head_size, count, info_size, reserved, length = (
        HEAD.unpack_from(data)
    )
    assert magic == MAGIC
    assert version == VERSION
    assert origin == ORIGIN
    assert head_size == HEAD.size == 36
    assert info_size == FILE_INFO.size == 160
    assert reserved == 0
    assert length == len(data)

    expected_offset = head_size + count * info_size
    assert expected_offset <= len(data)
    entries = []
    for index in range(count):
        info_offset = head_size + index * info_size
        (
            info_magic,
            raw_name,
            file_type,
            reserved16,
            reserved32,
            file_length,
            file_offset,
        ) = FILE_INFO.unpack_from(data, info_offset)
        assert info_magic == MAGIC
        assert reserved16 == 0
        assert reserved32 == 0
        assert file_offset == expected_offset
        assert file_offset + file_length <= len(data)
        nul = raw_name.find(b"\0")
        assert nul > 0
        name = raw_name[:nul].decode("utf-8")
        payload = data[file_offset : file_offset + file_length]
        child = decode_rep(payload) if file_type == TYPE_NPU_REP else None
        entries.append(RepEntry(name, file_type, payload, file_offset, child))
        expected_offset += file_length
    assert expected_offset == len(data)
    return DecodedRep(entries)


def extract_path(stderr: str, key: str) -> Path:
    prefix = f"npu-compute: {key}= "
    values = [
        Path(line[len(prefix) :])
        for line in stderr.splitlines()
        if line.startswith(prefix)
    ]
    assert len(values) == 1, stderr
    return values[0]


def run_fixture(
    tmp_path: Path,
    exit_code: Optional[int] = None,
    export_path: Optional[Path] = None,
    work_directory: Optional[Path] = None,
    mode: Optional[str] = None,
    no_pack: bool = False,
):
    if work_directory is None:
        work_directory = tmp_path / "work"
    work_directory.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["TMPDIR"] = str(tmp_path / "missing-tmp")
    environment["NPU_COMPUTE_DEBUG"] = "1"
    command = [
        str(CLI),
        "--section",
        "PipeUtilization",
    ]
    if export_path is not None:
        command.extend(("--export", str(export_path)))
    if no_pack:
        command.append("--no-pack")
    command.append(str(FIXTURE_APP))
    if mode is not None:
        command.extend(("--mode", mode))
    if exit_code is not None:
        command.extend(("--exit-code", str(exit_code)))
    result = subprocess.run(
        command,
        cwd=work_directory,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    return result, work_directory


@pytest.mark.parametrize("mode", ("missing-hardware", "incomplete", "invalid-csv"))
def test_partial_collection_preserves_valid_files_and_imports(tmp_path, mode):
    result, work = run_fixture(tmp_path, mode=mode)
    assert result.returncode == 3, result.stderr
    report = extract_path(result.stderr, "report")
    entries = {entry.name: entry for entry in decode_rep(report.read_bytes()).entries}
    assert entries["summary.jsonl"].payload == SUMMARY
    assert ("HardwareInfo.jsonl" in entries) == (mode != "missing-hardware")
    assert ("PipeUtilization.csv" in entries) == (mode != "invalid-csv")
    assert json.loads(entries["collection-status.json"].payload)["complete"] is False
    assert_no_collection_data_directories(work)
    imported = subprocess.run(
        [str(CLI), "--import", str(report)],
        cwd=work,
        text=True,
        capture_output=True,
        check=False,
    )
    assert imported.returncode == 0, imported.stderr
    restored = extract_path(imported.stderr, "unpacked")
    assert (restored / "summary.jsonl").read_bytes() == SUMMARY


def test_empty_collection_does_not_publish_status_only_report(tmp_path):
    result, work = run_fixture(tmp_path, mode="empty")
    assert result.returncode == 3, result.stderr
    assert list(work.glob("*.npu-rep")) == []


def test_protocol_failure_does_not_publish_already_received_files(tmp_path):
    report = tmp_path / "protocol.npu-rep"
    result = subprocess.run(
        [
            str(CLI),
            "--section",
            "Memory",
            "--export",
            str(report),
            str(FIXTURE_APP),
            "--mode",
            "protocol-bad-offset",
        ],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert result.returncode == 3, result.stderr
    assert "offset" in result.stderr and "UDS collection failed" in result.stderr
    assert not report.exists()
    assert_no_collection_data_directories(tmp_path)


@pytest.mark.parametrize(
    "mode", ["success", "csv-field-count", "csv-unit-conflict", "csv-directories"]
)
def test_csv_tables_are_automatically_packaged(tmp_path, mode):
    report = tmp_path / "sampling.npu-rep"
    result = subprocess.run(
        [
            str(CLI),
            "--section",
            "Memory",
            "--section",
            "PipeUtilization",
            "--export",
            str(report),
            str(FIXTURE_APP),
            "--mode",
            mode,
        ],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        timeout=30,
    )
    partial = mode in ("csv-field-count", "csv-unit-conflict")
    assert result.returncode == (3 if partial else 0), result.stderr
    assert report.is_file()
    output = tmp_path / "restored"
    output.mkdir()
    imported = subprocess.run(
        [str(CLI), "--import", str(report), "--export", str(output)],
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert imported.returncode == 0, imported.stderr
    databases = list(output.rglob("summary.db"))
    assert len(databases) == (2 if mode == "csv-directories" else 1)
    main = next(path for path in databases if path.parent.name != "collection-p1-0001")
    with sqlite3.connect(main) as database:
        tables = {
            row[0]
            for row in database.execute(
                "SELECT name FROM sqlite_master WHERE type='table'"
            )
        }
        expected = {"Memory", "PipeUtilization", "FieldUnitMap"}
        if partial:
            expected.remove(
                "PipeUtilization" if mode == "csv-unit-conflict" else "Memory"
            )
        assert tables == expected
        if "Memory" in tables:
            assert database.execute("SELECT * FROM Memory").fetchall() == [
                (0, 2 if mode == "csv-unit-conflict" else 128)
            ]
        if "PipeUtilization" in tables:
            assert database.execute("SELECT * FROM PipeUtilization").fetchall() == [
                (0, 75)
            ]
    assert len(list(output.rglob("Memory.csv"))) == (
        2 if mode == "csv-directories" else 1
    )
    if mode == "csv-field-count":
        for text in (
            "Memory.csv",
            "Memory",
            "record 2",
            "physical line 2",
            "expected 2 fields",
            "found 1",
        ):
            assert text in result.stderr, result.stderr
    if mode == "csv-unit-conflict":
        for text in ("PipeUtilization.csv", "latency", "us", "ms"):
            assert text in result.stderr, result.stderr
    if mode == "csv-directories":
        child = next(
            path for path in databases if path.parent.name == "collection-p1-0001"
        )
        with sqlite3.connect(child) as database:
            assert database.execute("SELECT * FROM Memory").fetchall() == [(1, 256)]
            assert database.execute("SELECT * FROM PipeUtilization").fetchall() == [
                (0, 75)
            ]
            assert {
                row[0]
                for row in database.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                )
            } == {"Memory", "PipeUtilization", "FieldUnitMap"}


def assert_no_collection_data_directories(work_directory: Path):
    assert list(work_directory.glob("npu-compute-*")) == []


def assert_no_temporary_report_files(work_directory: Path):
    assert list(work_directory.glob(".*.npu-rep.tmp.*")) == []


def assert_import_output_directory(path: Path, output_root: Path):
    assert path.is_absolute() and path.is_dir()
    assert path.parent == output_root.resolve()
    assert re.fullmatch(r"npu-compute-import-[0-9]+-[0-9]+-[A-Za-z0-9]{6}", path.name)


def assert_imported_fixture_files(output: Path):
    files = sorted(
        path.relative_to(output).as_posix()
        for path in output.rglob("*")
        if path.is_file()
    )
    assert files == [
        "HardwareInfo.jsonl",
        "PipeUtilization.csv",
        "summary.db",
        "summary.jsonl",
    ]
    assert (output / "HardwareInfo.jsonl").read_bytes() == HARDWARE_INFO
    assert (output / "PipeUtilization.csv").read_bytes() == PIPE_CSV
    assert (output / "summary.jsonl").read_bytes() == SUMMARY
    with sqlite3.connect(output / "summary.db") as database:
        assert database.execute("SELECT * FROM PipeUtilization").fetchall() == [(0, 75)]
    assert not (output / ".hardware_info.lock").exists()


def test_cli_recursively_packages_fixture_files(tmp_path):
    result, work_directory = run_fixture(tmp_path)

    assert result.returncode == 0, result.stderr
    assert "[aclpti]" not in result.stderr
    assert "[prof_api_stub]" not in result.stderr
    report = extract_path(result.stderr, "report")
    assert "npu-compute: data-directory=" not in result.stderr
    assert_no_collection_data_directories(work_directory)
    assert report.is_absolute() and report.is_file()
    assert report.parent == work_directory.resolve()
    assert re.fullmatch(r"report_[0-9]+_[0-9a-f]{8}\.npu-rep", report.name)
    assert list(work_directory.glob("*.npu-rep")) == [report]
    assert_no_temporary_report_files(work_directory)
    top = decode_rep(report.read_bytes())
    assert [entry.name for entry in top.entries] == [
        "HardwareInfo.jsonl",
        "PipeUtilization.csv",
        "summary.db",
        "summary.jsonl",
    ]
    assert [entry.file_type for entry in top.entries] == [
        TYPE_JSONL,
        TYPE_CSV,
        TYPE_SQLITE,
        TYPE_JSONL,
    ]
    assert top.entries[0].offset == HEAD.size + 4 * FILE_INFO.size
    assert top.entries[0].payload == HARDWARE_INFO
    assert top.entries[1].payload == PIPE_CSV
    assert top.entries[2].payload.startswith(b"SQLite format 3\0")
    assert top.entries[3].payload == SUMMARY


def test_cli_uses_explicit_report_file_and_existing_report_directory(tmp_path):
    explicit_work = tmp_path / "explicit-work"
    explicit_report = tmp_path / "reports" / "explicit.npu-rep"
    explicit_report.parent.mkdir()
    explicit_result, _ = run_fixture(
        tmp_path, export_path=explicit_report, work_directory=explicit_work
    )

    assert explicit_result.returncode == 0, explicit_result.stderr
    assert extract_path(explicit_result.stderr, "report") == explicit_report
    assert "npu-compute: data-directory=" not in explicit_result.stderr
    assert_no_collection_data_directories(explicit_work)
    assert explicit_report.is_file()
    assert_no_temporary_report_files(explicit_report.parent)

    directory_work = tmp_path / "directory-work"
    report_directory = tmp_path / "report-directory"
    report_directory.mkdir()
    directory_result, _ = run_fixture(
        tmp_path, export_path=report_directory, work_directory=directory_work
    )

    assert directory_result.returncode == 0, directory_result.stderr
    report = extract_path(directory_result.stderr, "report")
    assert report.parent == report_directory
    assert report.is_file()
    assert re.fullmatch(r"report_[0-9]+_[0-9a-f]{8}\.npu-rep", report.name)
    assert "npu-compute: data-directory=" not in directory_result.stderr
    assert_no_collection_data_directories(directory_work)
    assert_no_temporary_report_files(report_directory)


def test_cli_import_recursively_restores_report_files(tmp_path):
    result, work_directory = run_fixture(tmp_path)

    assert result.returncode == 0, result.stderr
    report = extract_path(result.stderr, "report")
    output_root = tmp_path / "unpacked"
    output_root.mkdir()
    keep = output_root / "keep.txt"
    keep.write_bytes(b"keep")
    environment = os.environ.copy()
    environment["TMPDIR"] = str(tmp_path / "missing-import-tmp")
    environment["ACL_API_INJECTION"] = "/npu-compute-import-must-not-load.so"
    command = [str(CLI), "--import", str(report), "--export", str(output_root)]
    first_import = subprocess.run(
        command,
        cwd=work_directory,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    second_import = subprocess.run(
        command,
        cwd=work_directory,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )

    assert first_import.returncode == 0, first_import.stderr
    assert second_import.returncode == 0, second_import.stderr
    first_output = extract_path(first_import.stderr, "unpacked")
    second_output = extract_path(second_import.stderr, "unpacked")
    assert first_output != second_output
    for imported, output in (
        (first_import, first_output),
        (second_import, second_output),
    ):
        assert_import_output_directory(output, output_root)
        assert "npu-compute: data-directory=" not in imported.stderr
        assert "npu-compute: report=" not in imported.stderr
        assert "[prof_api_stub]" not in imported.stderr
        assert "[aclpti]" not in imported.stderr
        assert_imported_fixture_files(output)
    assert keep.read_bytes() == b"keep"
    assert list(output_root.glob(".npu-compute-import-*")) == []


def test_failed_app_does_not_publish_report(tmp_path):
    result, work_directory = run_fixture(tmp_path, exit_code=17)

    assert result.returncode == 17
    assert "npu-compute: data-directory=" not in result.stderr
    assert_no_collection_data_directories(work_directory)
    assert "npu-compute: report=" not in result.stderr
    assert list(work_directory.glob("*.npu-rep")) == []
    assert_no_temporary_report_files(work_directory)
    assert "APP exited with status 17" in result.stderr


def test_sequential_collections_remove_data_directories_and_use_unique_reports(
    tmp_path,
):
    work_directory = tmp_path / "work"
    first_result, _ = run_fixture(tmp_path, work_directory=work_directory)
    second_result, _ = run_fixture(tmp_path, work_directory=work_directory)

    assert first_result.returncode == 0, first_result.stderr
    assert second_result.returncode == 0, second_result.stderr
    reports = {
        extract_path(first_result.stderr, "report"),
        extract_path(second_result.stderr, "report"),
    }
    assert len(reports) == 2
    assert "npu-compute: data-directory=" not in first_result.stderr
    assert "npu-compute: data-directory=" not in second_result.stderr
    assert_no_collection_data_directories(work_directory)
    for report in reports:
        assert report.parent == work_directory
        assert report.is_file()
    assert_no_temporary_report_files(work_directory)


def test_concurrent_collections_remove_data_directories_and_use_unique_reports(
    tmp_path,
):
    work_directory = tmp_path / "work"
    work_directory.mkdir()
    environment = os.environ.copy()
    environment["TMPDIR"] = str(tmp_path / "missing-tmp")
    command = [str(CLI), "--section", "PipeUtilization", str(FIXTURE_APP)]
    processes = [
        subprocess.Popen(
            command,
            cwd=work_directory,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        for _ in range(2)
    ]
    results = [process.communicate() for process in processes]

    standard_errors = []
    for process, (_, standard_error) in zip(processes, results):
        assert process.returncode == 0, standard_error
        standard_errors.append(standard_error)

    reports = [
        extract_path(standard_error, "report") for standard_error in standard_errors
    ]
    assert len(set(reports)) == 2
    for standard_error in standard_errors:
        assert "npu-compute: data-directory=" not in standard_error
    assert_no_collection_data_directories(work_directory)
    for report in reports:
        assert report.parent == work_directory
        assert report.is_file()
    assert_no_temporary_report_files(work_directory)


@pytest.mark.parametrize(
    "mode", ("performance-hints", "chunked-hints", "hints-before-exit")
)
def test_performance_hints_print_after_collection_and_are_preserved_on_import(
    tmp_path, mode
):
    result, work = run_fixture(tmp_path, mode=mode)
    assert result.returncode == 0, result.stderr
    assert result.stdout.count("npu-compute: Performance Hints") == 1
    assert "  op=fixture_kernel  device=0" in result.stdout
    assert result.stdout.count("[PH03][AIV]") == 1
    assert "[PH03][AIV]" not in result.stderr
    if mode == "hints-before-exit":
        assert result.stdout.index("fixture-exiting") < result.stdout.index(
            "[PH03][AIV]"
        )
    report = extract_path(result.stderr, "report")
    entries = {entry.name: entry for entry in decode_rep(report.read_bytes()).entries}
    records = [
        json.loads(line) for line in entries["summary.jsonl"].payload.splitlines()
    ]
    assert records[-2]["category"] == "PerformanceHints"
    assert len(records[-2]["hints"]) == 1
    imported = subprocess.run(
        [str(CLI), "--import", str(report)],
        cwd=work,
        text=True,
        capture_output=True,
        check=False,
    )
    assert imported.returncode == 0, imported.stderr
    assert "Performance Hints" not in imported.stdout
    assert "[PH03][AIV]" not in imported.stdout
    restored = extract_path(imported.stderr, "unpacked")
    assert (restored / "summary.jsonl").read_bytes() == entries["summary.jsonl"].payload


def test_no_pack_prints_performance_hints_after_collection(tmp_path):
    result, _ = run_fixture(tmp_path, mode="hints-before-exit", no_pack=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout.count("npu-compute: Performance Hints") == 1
    assert "  op=fixture_kernel  device=0" in result.stdout
    assert result.stdout.count("[PH03][AIV]") == 1
    assert result.stdout.index("fixture-exiting") < result.stdout.index("[PH03][AIV]")
    output = extract_path(result.stderr, "directory")
    records = [
        json.loads(line) for line in (output / "summary.jsonl").read_text().splitlines()
    ]
    assert records[-2]["category"] == "PerformanceHints"
    assert len(records[-2]["hints"]) == 1


@pytest.mark.parametrize("mode", ("empty-hints", "invalid-hints"))
@pytest.mark.parametrize("no_pack", (False, True))
def test_empty_or_invalid_performance_hints_do_not_print(tmp_path, mode, no_pack):
    result, _ = run_fixture(tmp_path, mode=mode, no_pack=no_pack)
    assert result.returncode == (0 if mode == "empty-hints" else 3), result.stderr
    assert "Performance Hints" not in result.stdout
    assert "[PH" not in result.stdout
    assert (
        "empty_hints" if mode == "empty-hints" else "invalid summary JSONL"
    ) in result.stderr
