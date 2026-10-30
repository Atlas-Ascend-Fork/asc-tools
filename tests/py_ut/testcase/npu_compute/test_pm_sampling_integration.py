# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""CLI/report integration; real gather runs require explicit REAL_CLI/REAL_APP paths."""

import json
import csv
import os
import re
import sqlite3
import struct
import subprocess
from collections import defaultdict
from pathlib import Path

import pytest


COMBINATIONS = [("PmSampling",), ("Memory", "PmSampling"), ("Pipeline", "PmSampling")]
HEAD = struct.Struct("<8sIHHIIIQ")
INFO = struct.Struct("<8s128sHHIQQ")
NAMES = {"summary.db": 5, "objdump.txt": 6, "symbolizer.json": 2}


def report_files(data, prefix=""):
    magic, version, origin, head_size, count, info_size, reserved, length = (
        HEAD.unpack_from(data)
    )
    assert magic == b"npu-rep\0" and version == 0x10000
    assert origin == 1 and head_size == HEAD.size and info_size == INFO.size
    assert reserved == 0 and length == len(data)
    files = {}
    offset = head_size + count * info_size
    for index in range(count):
        magic, raw_name, kind, reserved16, reserved32, size, position = (
            INFO.unpack_from(data, head_size + index * info_size)
        )
        assert magic == b"npu-rep\0" and reserved16 == reserved32 == 0
        assert position == offset and size <= len(data) - position
        name = raw_name.split(b"\0", 1)[0].decode()
        assert name and "/" not in name and name not in (".", "..")
        payload = data[position : position + size]
        if kind == 1:
            children = report_files(payload, prefix + name + "/")
            assert files.keys().isdisjoint(children)
            files.update(children)
        else:
            assert prefix + name not in files
            files[prefix + name] = (kind, payload)
        offset += size
    assert offset == len(data)
    return files


def run(command, directory, environment):
    result = subprocess.run(
        command,
        cwd=directory,
        env=environment,
        text=True,
        capture_output=True,
        timeout=240,
    )
    (directory / "command.json").write_text(json.dumps(command, indent=2))
    (directory / "stdout.txt").write_text(result.stdout)
    (directory / "stderr.txt").write_text(result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    return result


def check_report(cli, directory, sections, environment):
    report = directory / "sampling.npu-rep"
    assert report.is_file()
    assert struct.unpack_from("<I", report.read_bytes(), 8)[0] == 0x10000
    files = report_files(report.read_bytes())
    expected = {"HardwareInfo.jsonl", "summary.jsonl", *NAMES}
    if "Memory" in sections:
        expected.add("Memory.csv")
    if "Pipeline" in sections:
        expected.add("PipeTrace.json")
    assert set(files) == expected
    records = [json.loads(line) for line in files["summary.jsonl"][1].splitlines()]
    assert records[-2]["category"] == "PerformanceHints"
    assert records[-1]["category"] == "OpInfoSummary"
    if sections == ("PmSampling",):
        assert len(records) == 2
        assert records[0] == {"category": "PerformanceHints", "hints": []}
    for name, kind in NAMES.items():
        assert files[name][0] == kind and files[name][1]
    imported = directory / "imported"
    imported.mkdir()
    result = subprocess.run(
        [str(cli), "--import", str(report), "--export", str(imported)],
        env=environment,
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stderr
    databases = list(imported.rglob("summary.db"))
    assert len(databases) == 1
    root = databases[0].parent
    for name, (_, payload) in files.items():
        assert (root / name).read_bytes() == payload
    with sqlite3.connect(databases[0].as_uri() + "?mode=ro", uri=True) as database:
        assert database.execute("PRAGMA integrity_check").fetchall() == [("ok",)]
        columns = [row[1] for row in database.execute("PRAGMA table_info(PcSampling)")]
        assert columns == [
            "address",
            "ibuf_empty",
            "nop_cycles",
            "scoreboard_not_ready",
            "register_bank_conflict",
            "resource_conflict",
            "warp_level_sync",
            "divergence_stack_spill",
            "others",
        ]
        rows = dict(
            (row[0], tuple(row[1:]))
            for row in database.execute("SELECT * FROM PcSampling")
        )
        assert rows
        assert all(
            len(counts) == 8
            and all(isinstance(value, int) and value >= 0 for value in counts)
            for counts in rows.values()
        )
        if "Memory" in sections:
            with (root / "Memory.csv").open(newline="") as input_file:
                csv_rows = list(csv.reader(input_file))
            assert (
                database.execute("SELECT count(*) FROM Memory").fetchone()[0]
                == len(csv_rows) - 1
            )
            tables = {
                row[0]
                for row in database.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                )
            }
            assert {"PcSampling", "Memory", "FieldUnitMap"} <= tables
    stack = json.loads((root / "symbolizer.json").read_text())
    assert len(stack) == len(rows) and {
        int(record["Address"], 16) for record in stack
    } == set(rows)
    assert all(isinstance(record["Symbol"], list) for record in stack)
    addresses = {
        int(value, 16)
        for value in re.findall(
            r"^\s*([0-9a-fA-F]+):", (root / "objdump.txt").read_text(), re.M
        )
    }
    assert all(pc in addresses for pc in rows)
    if "Memory" in sections:
        assert len((root / "Memory.csv").read_text().splitlines()) >= 2
    if "Pipeline" in sections:
        assert json.loads((root / "PipeTrace.json").read_text())["traceEvents"]
    assert not list(directory.glob("npu-compute-*")), (
        "temporary collection directories were not cleaned"
    )
    return rows


@pytest.mark.parametrize("sections", COMBINATIONS)
def test_stub_report_round_trip(tmp_path, sections):
    cli_value = os.environ.get("NPU_COMPUTE_TEST_CLI")
    app_value = os.environ.get("NPU_COMPUTE_REP_FIXTURE_APP")
    if not cli_value or not app_value:
        pytest.skip("set NPU_COMPUTE_TEST_CLI and NPU_COMPUTE_REP_FIXTURE_APP")
    command = [cli_value]
    for section in sections:
        command.extend(("--section", section))
    command.extend(
        (
            "--export",
            str(tmp_path / "sampling.npu-rep"),
            app_value,
            "--mode",
            "pm-sampling",
        )
    )
    run(command, tmp_path, os.environ.copy())
    rows = check_report(Path(cli_value), tmp_path, sections, os.environ.copy())
    assert rows == {0x268: (0, 215, 16320, 0, 0, 0, 0, 0)}


def check_raw_counts(log, rows):
    contexts = {}
    for replay, entry, aic, aiv in re.findall(
        r"PmSampling context replay=(\d+) elfEntry=(0x[0-9a-f]+) aicEntry=(0x[0-9a-f]+) aivEntry=(0x[0-9a-f]+)",
        log,
    ):
        contexts[int(replay)] = (int(entry, 16), int(aic, 16), int(aiv, 16))
    assert len(contexts) == 1, "official gather has one sampled launch"
    totals = defaultdict(lambda: [0] * 8)
    records = 0
    active_replay = None
    expected_bytes = actual_bytes = 0
    for line in log.splitlines():
        header = re.search(
            r"\[DEBUG-rawdata\] replay=(\d+) type=(\d+).*validBytes=(\d+)", line
        )
        if header:
            assert actual_bytes == expected_bytes, "raw payload log is incomplete"
            active_replay = int(header[1]) if int(header[2]) == 9 else None
            expected_bytes = int(header[3]) if active_replay is not None else 0
            actual_bytes = 0
        payload = re.search(r"payload replay=(\d+) offset=\d+ bytes=([0-9a-f ]+)", line)
        if active_replay is None or not payload:
            continue
        assert int(payload[1]) == active_replay
        data = bytes.fromhex(payload[2])
        assert len(data) == 16, (
            "gather fixture expects aligned 16-byte callback records"
        )
        actual_bytes += len(data)
        entry, aic, aiv = contexts[active_replay]
        encoded = int.from_bytes(data[:3], "little") * 8
        candidates = {
            (encoded - ((runtime - entry) & 0x7FFFFFF)) & 0x7FFFFFF
            for runtime in (aic, aiv)
            if runtime
        }
        # The fixture's validated companion-to-original map is 0x10458 -> 0x268.
        candidates = {0x268 if value == 0x10458 else value for value in candidates}
        matched = candidates & rows.keys()
        assert len(matched) == 1, (
            "gather sample did not map uniquely to a reported business PC"
        )
        pc = matched.pop()
        for index, count in enumerate(data[3:11]):
            totals[pc][index] += count
        records += 1
    assert actual_bytes == expected_bytes and records > 0
    assert {pc: tuple(counts) for pc, counts in totals.items()} == rows
    return records


@pytest.mark.parametrize("sections", COMBINATIONS)
def test_real_gather_round_trip(tmp_path, sections):
    cli_value = os.environ.get("NPU_COMPUTE_PM_SAMPLING_REAL_CLI")
    app_value = os.environ.get("NPU_COMPUTE_PM_SAMPLING_REAL_APP")
    if not cli_value or not app_value:
        pytest.skip(
            "set NPU_COMPUTE_PM_SAMPLING_REAL_CLI and NPU_COMPUTE_PM_SAMPLING_REAL_APP after sourcing CANN"
        )
    environment = os.environ.copy()
    environment["NPU_COMPUTE_DEBUG"] = "1"
    command = [cli_value]
    for section in sections:
        command.extend(("--section", section))
    command.extend(("--export", str(tmp_path / "sampling.npu-rep"), app_value))
    result = run(command, tmp_path, environment)
    assert "[Success] Case accuracy is verification passed." in result.stdout
    rows = check_report(Path(cli_value), tmp_path, sections, environment)
    records = check_raw_counts(result.stderr, rows)
    (tmp_path / "verification.json").write_text(
        json.dumps({"sections": sections, "records": records, "rows": rows}, indent=2)
    )


@pytest.mark.parametrize(
    "failure",
    [
        "missing-db",
        "missing-objdump",
        "missing-symbolizer",
        "missing-summary",
        "invalid-db",
        "invalid-objdump",
        "invalid-symbolizer",
        "missing-all",
    ],
)
def test_partial_sampling_report(tmp_path, failure):
    cli = os.environ["NPU_COMPUTE_TEST_CLI"]
    app = os.environ["NPU_COMPUTE_REP_FIXTURE_APP"]
    result = subprocess.run(
        [
            cli,
            "--section",
            "Memory",
            "--section",
            "PmSampling",
            "--export",
            str(tmp_path / "partial.npu-rep"),
            app,
            "--mode",
            "pm-sampling-" + failure,
        ],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        timeout=30,
    )
    missing_sampling_file = failure in {
        "missing-db",
        "missing-objdump",
        "missing-symbolizer",
        "missing-all",
    }
    assert result.returncode == (0 if missing_sampling_file else 3), (
        result.stdout + result.stderr
    )
    if missing_sampling_file:
        assert "WARNING" in result.stderr
        assert "collection incomplete" not in result.stderr
    else:
        assert "collection incomplete" in result.stderr
    files = report_files((tmp_path / "partial.npu-rep").read_bytes())
    assert {"HardwareInfo.jsonl", "Memory.csv"} <= files.keys()
    if failure == "missing-summary":
        assert "summary.jsonl" not in files
    else:
        assert "summary.jsonl" in files
    omitted = {
        "db": "summary.db",
        "objdump": "objdump.txt",
        "symbolizer": "symbolizer.json",
        "summary": "summary.jsonl",
    }
    expected = (
        set()
        if failure == "missing-all"
        else set(NAMES) - {omitted[failure.split("-")[1]]}
    )
    if failure in ("missing-db", "missing-all"):
        expected.add(
            "summary.db"
        )  # Memory still produces its own database, without PcSampling.
        database_path = tmp_path / "csv-only.db"
        database_path.write_bytes(files["summary.db"][1])
        with sqlite3.connect(database_path) as database:
            tables = {
                row[0]
                for row in database.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                )
            }
            assert tables == {"Memory", "FieldUnitMap"}
    assert set(NAMES) & files.keys() == expected
    if failure == "missing-all":
        for name in ("summary.db", "objdump.txt", "symbolizer.json"):
            assert name in result.stderr
    else:
        assert omitted[failure.split("-")[1]] in result.stderr


def test_simd_sampling_succeeds_without_pc_data_and_retains_csv_database(tmp_path):
    result = subprocess.run(
        [
            os.environ["NPU_COMPUTE_TEST_CLI"],
            "--section",
            "Memory",
            "--section",
            "PmSampling",
            "--export",
            str(tmp_path / "simd.npu-rep"),
            os.environ["NPU_COMPUTE_REP_FIXTURE_APP"],
            "--mode",
            "pm-sampling-simd",
        ],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "requires SIMT" not in result.stderr
    assert "collection incomplete" not in result.stderr
    assert "[ERROR]" not in result.stderr
    files = report_files((tmp_path / "simd.npu-rep").read_bytes())
    assert {"Memory.csv", "summary.db", "objdump.txt"} <= files.keys()
    path = tmp_path / "simd.db"
    path.write_bytes(files["summary.db"][1])
    with sqlite3.connect(path) as database:
        tables = {row[0] for row in database.execute("SELECT name FROM sqlite_master")}
        assert "Memory" in tables and "PcSampling" not in tables
