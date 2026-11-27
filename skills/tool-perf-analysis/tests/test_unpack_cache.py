# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.

"""Regression coverage for report cache reuse through the facts CLI."""

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "extract_facts.py"
MAGIC = b"npu-rep\0"
HEADER_FMT = "<8sIHHIIIQ"
ENTRY_FMT = "<8s128sHHIQQ"


def _rep(entries):
    header_size = struct.calcsize(HEADER_FMT)
    entry_size = struct.calcsize(ENTRY_FMT)
    offset = header_size + entry_size * len(entries)
    table = []
    payloads = []
    for name, kind, payload in entries:
        table.append(
            struct.pack(
                ENTRY_FMT, MAGIC, name.encode(), kind, 0, 0, len(payload), offset
            )
        )
        payloads.append(payload)
        offset += len(payload)
    header = struct.pack(
        HEADER_FMT, MAGIC, 0x10000, 1, header_size, len(entries), entry_size, 0, offset
    )
    return header + b"".join(table) + b"".join(payloads)


def _summary(name):
    return (json.dumps({"category": "OpInfoSummary", "Op Name": name}) + "\n").encode()


def _run(report, cache, output):
    return subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--rep",
            str(report),
            "--workdir",
            str(cache),
            "-o",
            str(output),
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=30,
    )


def _op(output):
    return json.loads(output.read_text(encoding="utf-8"))["op"]["Op Name"]


def test_nested_report_returns_same_operation_on_cache_reuse(tmp_path):
    report, cache, output = (
        tmp_path / "sample.npu-rep",
        tmp_path / "cache",
        tmp_path / "facts.json",
    )
    report.write_bytes(
        _rep([("op0.rep", 1, _rep([("summary.jsonl", 3, _summary("FIRST"))]))])
    )
    unpacked_paths = []
    for _ in range(2):
        result = _run(report, cache, output)
        assert result.returncode == 0, result.stderr
        assert _op(output) == "FIRST"
        unpacked_paths.append(
            json.loads(output.read_text(encoding="utf-8"))["unpacked_dir"]
        )
    assert unpacked_paths[0] == unpacked_paths[1]


def test_replaced_report_does_not_reuse_old_content(tmp_path):
    report, cache, output = (
        tmp_path / "sample.npu-rep",
        tmp_path / "cache",
        tmp_path / "facts.json",
    )
    for name in ("FIRST", "SECOND"):
        report.write_bytes(_rep([("summary.jsonl", 3, _summary(name))]))
        result = _run(report, cache, output)
        assert result.returncode == 0, result.stderr
        assert _op(output) == name


def test_incomplete_cache_is_rebuilt_without_deleting_existing_files(tmp_path):
    report, cache, output = (
        tmp_path / "sample.npu-rep",
        tmp_path / "cache",
        tmp_path / "facts.json",
    )
    payload = _rep([("summary.jsonl", 3, _summary("FIRST"))])
    report.write_bytes(payload)
    incomplete = cache / ("rep-" + hashlib.sha256(payload).hexdigest())
    incomplete.mkdir(parents=True)
    sentinel = incomplete / "partial.txt"
    sentinel.write_text("preserve", encoding="utf-8")
    result = _run(report, cache, output)
    assert result.returncode == 0, result.stderr
    assert _op(output) == "FIRST"
    assert sentinel.read_text(encoding="utf-8") == "preserve"


def test_missing_or_empty_cached_directory_is_rebuilt(tmp_path):
    report, cache, output = (
        tmp_path / "sample.npu-rep",
        tmp_path / "cache",
        tmp_path / "facts.json",
    )
    payload = _rep([("summary.jsonl", 3, _summary("FIRST"))])
    report.write_bytes(payload)
    assert _run(report, cache, output).returncode == 0
    marker = cache / ("rep-" + hashlib.sha256(payload).hexdigest() + ".complete")
    directory = cache / json.loads(marker.read_text(encoding="utf-8"))["directory"]
    (directory / "summary.jsonl").unlink()
    result = _run(report, cache, output)
    assert result.returncode == 0, result.stderr
    assert _op(output) == "FIRST"
    assert directory.is_dir()


def test_concurrent_extractions_publish_complete_cache(tmp_path):
    report, cache = tmp_path / "sample.npu-rep", tmp_path / "cache"
    report.write_bytes(_rep([("summary.jsonl", 3, _summary("FIRST"))]))
    with ThreadPoolExecutor(max_workers=2) as executor:
        futures = [
            executor.submit(_run, report, cache, tmp_path / f"facts-{i}.json")
            for i in range(2)
        ]
        results = [future.result() for future in futures]
    assert all(result.returncode == 0 for result in results), [
        r.stderr for r in results
    ]
    assert [_op(tmp_path / f"facts-{i}.json") for i in range(2)] == ["FIRST", "FIRST"]
    result = _run(report, cache, tmp_path / "facts-reuse.json")
    assert result.returncode == 0, result.stderr
    assert _op(tmp_path / "facts-reuse.json") == "FIRST"


def test_missing_cached_file_is_rebuilt_when_other_files_remain(tmp_path):
    report, cache, output = (
        tmp_path / "sample.npu-rep",
        tmp_path / "cache",
        tmp_path / "facts.json",
    )
    report.write_bytes(
        _rep(
            [
                ("summary.jsonl", 3, _summary("FIRST")),
                ("extra.csv", 3, b"field,value\ncount,1\n"),
            ]
        )
    )
    result = _run(report, cache, output)
    assert result.returncode == 0, result.stderr
    assert _op(output) == "FIRST"
    directory = Path(json.loads(output.read_text(encoding="utf-8"))["unpacked_dir"])
    (directory / "summary.jsonl").unlink()
    assert (directory / "extra.csv").is_file()
    result = _run(report, cache, output)
    assert result.returncode == 0, result.stderr
    facts = json.loads(output.read_text(encoding="utf-8"))
    assert (facts.get("op") or {}).get("Op Name") == "FIRST", facts.get("op")
    assert (directory / "extra.csv").is_file()


def main():
    cases = (
        test_nested_report_returns_same_operation_on_cache_reuse,
        test_replaced_report_does_not_reuse_old_content,
        test_incomplete_cache_is_rebuilt_without_deleting_existing_files,
        test_missing_or_empty_cached_directory_is_rebuilt,
        test_concurrent_extractions_publish_complete_cache,
        test_missing_cached_file_is_rebuilt_when_other_files_remain,
    )
    for case in cases:
        with tempfile.TemporaryDirectory() as temporary:
            case(Path(temporary))
        print(f"PASS {case.__name__}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
