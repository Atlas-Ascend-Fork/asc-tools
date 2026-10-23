# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""SQLite optional runtime loading and partial report publication."""

import os
from pathlib import Path
import shutil
import sqlite3
import subprocess

import pytest


BUILD = Path(
    os.environ.get("NPU_COMPUTE_BUILD_DIR", "/tmp/asc_tools_pm_sampling_verify")
)
TEST_DIR = BUILD / "tests/ut/testcase/npu_tools/npu_compute/tests"
PROBE = TEST_DIR / "npu_compute_sqlite_runtime_loader_test"
STUB = TEST_DIR / "libnpu_compute_sqlite_loader_stub.so"
INCOMPLETE = TEST_DIR / "libnpu_compute_sqlite_incomplete_stub.so"
CLI = Path(
    os.environ.get(
        "NPU_COMPUTE_TEST_CLI",
        str(BUILD / "tests/ut/testcase/npu_tools/bin/npu-compute"),
    )
)
APP = Path(
    os.environ.get(
        "NPU_COMPUTE_REP_FIXTURE_APP", str(CLI.parent / "npu_compute_rep_fixture_app")
    )
)


def environment(mode=None, library=None):
    env = os.environ.copy()
    runtime_dir = os.environ.get(
        "NPU_COMPUTE_TEST_RUNTIME_DIR",
        str(BUILD / "tests/ut/testcase/npu_tools/fake-cann/x86_64-linux/lib64"),
    )
    env["LD_LIBRARY_PATH"] = runtime_dir + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    env["LD_PRELOAD"] = str(STUB) + (
        ":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else ""
    )
    if mode:
        env["NPU_COMPUTE_SQLITE_STUB_MODE"] = mode
    else:
        env.pop("NPU_COMPUTE_SQLITE_STUB_MODE", None)
    if library:
        env["NPU_COMPUTE_SQLITE_STUB_LIBRARY"] = str(library)
    return env


def invoke(mode, env, executable=PROBE):
    result = subprocess.run(
        [str(executable), mode], env=env, text=True, capture_output=True, timeout=30
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return result


def unavailable_environment(tmp_path, failure):
    library = INCOMPLETE
    if failure == "wrong-architecture":
        library = tmp_path / "wrong.so"
        library.write_bytes(b"\x7fELF\x01\x01\x01" + b"\0" * 249)
    return environment(failure, library)


def test_product_has_no_sqlite_hard_dependency():
    for path in (
        BUILD / "npu_tools/bin/npu-compute",
        BUILD / "npu_tools/lib64/libnpu_compute_processor.so",
    ):
        result = subprocess.run(
            ["readelf", "-d", str(path)], text=True, capture_output=True, check=True
        )
        assert "libsqlite3" not in result.stdout
        result = subprocess.run(
            ["nm", "-u", str(path)], text=True, capture_output=True, check=True
        )
        assert "sqlite3_" not in result.stdout


def test_sqlite_is_loaded_only_on_first_database_use():
    result = invoke("lazy", environment("absent"))
    assert "sqlite_loader_stub" not in result.stderr


@pytest.mark.parametrize("mode", ["ready", "concurrent"])
def test_real_sqlite_binding_and_first_use(mode):
    result = invoke(mode, environment())
    assert "SQLite ready:" in result.stdout


@pytest.mark.parametrize("failure", ["absent", "incomplete", "wrong-architecture"])
def test_unavailable_reason_and_failure_cache(tmp_path, failure):
    result = invoke("unavailable", unavailable_environment(tmp_path, failure))
    assert result.stderr.count("[sqlite_loader_stub] attempt=") == 4
    assert "SQLite runtime loading failed" in result.stdout
    assert "database output is unavailable" in result.stdout
    assert "CSV, disassembly and symbolizer" in result.stdout
    expected = {
        "absent": "sqlite-not-installed.so",
        "incomplete": "sqlite3_close",
        "wrong-architecture": "ELFCLASS32",
    }[failure]
    assert expected in result.stdout


def test_bad_candidate_is_closed_and_next_candidate_used():
    result = invoke("ready", environment("first-incomplete", INCOMPLETE))
    assert "SQLite ready:" in result.stdout
    assert result.stderr.count("[sqlite_loader_stub] attempt=") >= 2


def test_module_directory_library_is_preferred(tmp_path):
    # Resolve the actual environment library through the real loader.
    import ctypes
    import ctypes.util

    library = ctypes.CDLL(ctypes.util.find_library("sqlite3"))
    mappings = Path("/proc/self/maps").read_text().splitlines()
    path = next(Path(line.split()[-1]) for line in mappings if "/libsqlite3.so" in line)
    assert library
    executable = tmp_path / PROBE.name
    shutil.copyfile(PROBE, executable)
    executable.chmod(0o750)
    (tmp_path / "libsqlite3.so.0").symlink_to(path)
    result = invoke("ready", environment(), executable)
    assert "SQLite ready: " + str(tmp_path / "libsqlite3.so.0") in result.stdout


@pytest.mark.parametrize("failure", ["absent", "incomplete", "wrong-architecture"])
@pytest.mark.parametrize("sections", [("PmSampling",), ("Memory", "PmSampling")])
def test_unavailable_sqlite_keeps_other_sampling_files_and_report(
    tmp_path, failure, sections
):
    report = tmp_path / "sampling.npu-rep"
    env = unavailable_environment(tmp_path, failure)
    env["NPU_COMPUTE_SQLITE_STUB_EXCLUDE_FIXTURE"] = "1"
    command = [str(CLI)]
    for section in sections:
        command.extend(("--section", section))
    command.extend(("--export", str(report), str(APP), "--mode", "pm-sampling"))
    result = subprocess.run(
        command, cwd=tmp_path, env=env, text=True, capture_output=True, timeout=30
    )
    assert result.returncode == 3, result.stdout + result.stderr
    assert report.is_file()
    assert (
        "summary.db" in result.stderr
        and "SQLite runtime loading failed" in result.stderr
    )
    assert "excluded this file; other valid results are retained" in result.stderr
    expected = {
        "absent": "sqlite-not-installed.so",
        "incomplete": "sqlite3_close",
        "wrong-architecture": "ELFCLASS32",
    }[failure]
    assert expected in result.stderr
    restored = tmp_path / "restored"
    restored.mkdir()
    imported = subprocess.run(
        [str(CLI), "--import", str(report), "--export", str(restored)],
        env=env,
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert imported.returncode == 0, imported.stderr
    assert not list(restored.rglob("summary.db"))
    for name in ("objdump.txt", "symbolizer.json", "HardwareInfo.jsonl"):
        assert len(list(restored.rglob(name))) == 1
    assert len(list(restored.rglob("Memory.csv"))) == int("Memory" in sections)
    assert len(list(restored.rglob("summary.jsonl"))) == int("Memory" in sections)
    assert "sqlite_loader_stub" not in imported.stderr, "unpacking must not load SQLite"


@pytest.mark.parametrize("failure", ["absent", "incomplete", "wrong-architecture"])
def test_worker_unavailable_sqlite_keeps_disassembly_and_symbols(tmp_path, failure):
    worker = TEST_DIR / "npu_compute_pm_sampling_runtime_test"
    result = subprocess.run(
        [str(worker), "--child", "sqlite-unavailable", str(tmp_path)],
        env=unavailable_environment(tmp_path, failure),
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert not list(tmp_path.rglob("summary.db"))
    assert (tmp_path / "received/objdump.txt").is_file()
    assert (tmp_path / "received/symbolizer.json").is_file()
    assert "SQLite runtime loading failed" in result.stderr


@pytest.mark.parametrize("sampling", [False, True])
def test_database_commit_failure_keeps_original_and_other_files(tmp_path, sampling):
    report = tmp_path / "sampling.npu-rep"
    env = environment("commit-denied")
    env["NPU_COMPUTE_SQLITE_STUB_EXCLUDE_FIXTURE"] = "1"
    command = [str(CLI), "--section", "Memory"]
    if sampling:
        command.extend(("--section", "PmSampling"))
    command.extend(("--export", str(report), str(APP), "--mode", "pm-sampling"))
    result = subprocess.run(
        command, cwd=tmp_path, env=env, text=True, capture_output=True, timeout=30
    )
    assert result.returncode == 3, result.stderr
    assert report.is_file()
    for text in (
        "summary.db",
        "commit database transaction",
        "not authorized",
        "original database",
    ):
        assert text in result.stderr, result.stderr
    restored = tmp_path / "restored"
    restored.mkdir()
    imported = subprocess.run(
        [str(CLI), "--import", str(report), "--export", str(restored)],
        env=env,
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert imported.returncode == 0, imported.stderr
    assert len(list(restored.rglob("Memory.csv"))) == 1
    assert len(list(restored.rglob("summary.jsonl"))) == 1
    databases = list(restored.rglob("summary.db"))
    assert len(databases) == int(sampling)
    if sampling:
        with sqlite3.connect(databases[0]) as database:
            assert database.execute("SELECT * FROM PcSampling").fetchall() == [
                (0x268, 0, 215, 16320, 0, 0, 0, 0, 0)
            ]
            assert database.execute(
                "SELECT name FROM sqlite_master WHERE type='table'"
            ).fetchall() == [("PcSampling",)]
        for name in ("objdump.txt", "symbolizer.json"):
            assert len(list(restored.rglob(name))) == 1
