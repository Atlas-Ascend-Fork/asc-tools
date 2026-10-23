# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import csv
import json
import os
import subprocess
from pathlib import Path

import pytest


BIN_DIR = Path(os.environ["NPU_COMPUTE_TEST_BIN_DIR"])
CALLBACK_STUB = Path(os.environ["NPU_COMPUTE_CALLBACK_STUB"])
HARDWARE_API_STUB = Path(os.environ["NPU_COMPUTE_HARDWARE_API_STUB"])
CLI = BIN_DIR / "npu-compute"
APP = BIN_DIR / "npu_compute_stub_demo_app"


def test_list_sets_and_errors_do_not_launch(tmp_path):
    basic = [
        "Pipeline",
        "PipeUtilization",
        "Memory",
        "MemoryL0",
        "MemoryUB",
        "L2Cache",
        "ArithmeticUtilization",
    ]
    expected = ""
    for name, members in (
        ("basic", basic),
        ("full", [*basic, "ResourceConflictRatio", "PmSampling"]),
    ):
        expected += (
            name
            + ":\n"
            + "".join(
                "  " + member + "\n" for member in sorted(members, key=str.casefold)
            )
        )
    result = subprocess.run(
        [str(CLI), "--list-sets"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected
    assert result.stderr == ""
    assert list(tmp_path.iterdir()) == []
    for arguments, message in (
        (["--list-sets", str(APP)], "use --list-sets as a standalone command."),
        (["--list-sets=basic"], "--list-sets does not take a value."),
        (
            ["--set="],
            "--set requires a set name. Use --list-sets to see supported names.",
        ),
        (
            ["--set", "Basic", str(APP)],
            "unsupported set name 'Basic'. Names are case-sensitive; use --list-sets to see supported names.",
        ),
    ):
        result = subprocess.run(
            [str(CLI), *arguments],
            cwd=tmp_path,
            capture_output=True,
            text=True,
            timeout=10,
        )
        assert result.returncode == 2
        assert result.stderr == "[ERROR] npu-compute: " + message + "\n"
        assert result.stdout == ""
        assert list(tmp_path.iterdir()) == []


def extract_path(stderr, key):
    prefix = f"npu-compute: {key}= "
    values = [
        Path(line[len(prefix) :])
        for line in stderr.splitlines()
        if line.startswith(prefix)
    ]
    assert len(values) == 1, stderr
    return values[0]


def unpack_report(result, work_directory):
    report = extract_path(result.stderr, "report")
    output_root = work_directory / "unpacked"
    output_root.mkdir()
    imported = subprocess.run(
        [str(CLI), "--import", str(report), "--export", str(output_root)],
        cwd=work_directory,
        text=True,
        capture_output=True,
        timeout=60,
        check=False,
    )
    assert imported.returncode == 0, imported.stderr
    return extract_path(imported.stderr, "unpacked")


def test_cli_profapi_callback_collector_and_jsonl_end_to_end(tmp_path):
    work_directory = tmp_path / "callback-events"
    work_directory.mkdir()
    environment = os.environ.copy()
    existing_preload = environment.get("LD_PRELOAD", "")
    environment["LD_PRELOAD"] = os.pathsep.join(
        value
        for value in (str(CALLBACK_STUB), str(HARDWARE_API_STUB), existing_preload)
        if value
    )
    existing_library_path = environment.get("LD_LIBRARY_PATH", "")
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(
        value for value in (str(BIN_DIR), existing_library_path) if value
    )
    environment["INJECTION_TEST_CALLBACK_EVENTS"] = "1"

    result = subprocess.run(
        [str(CLI), "--section", "PipeUtilization", str(APP)],
        env=environment,
        cwd=work_directory,
        text=True,
        capture_output=True,
        timeout=60,
        check=False,
    )

    assert result.returncode == 3, result.stderr
    assert "[demo] completed" in result.stderr
    assert result.stderr.count("[acl_pti_callback_stub] subscribe") == 1
    hardware_info_trigger_cbids = (13, 0, 16, 17, 18, 3, 15, 14, 12)
    assert result.stderr.count("[acl_pti_callback_stub] enable=1") == len(
        hardware_info_trigger_cbids
    )
    assert result.stderr.count("[acl_pti_callback_stub] enable=0") == len(
        hardware_info_trigger_cbids
    )
    for cbid in hardware_info_trigger_cbids:
        assert (
            f"[acl_pti_callback_stub] enable=1 domain=1 cbid={cbid} result=0"
            in result.stderr
        )
        assert (
            f"[acl_pti_callback_stub] enable=0 domain=1 cbid={cbid} result=0"
            in result.stderr
        )
    assert result.stderr.count("[acl_pti_callback_stub] event") == 4
    for site in (0, 1):
        assert (
            f"[acl_pti_callback_stub] event domain=1 cbid=4 site={site} "
            "retval=0 dispatched=0" in result.stderr
        )
        assert (
            f"[acl_pti_callback_stub] event domain=1 cbid=13 site={site} "
            "retval=0 dispatched=1" in result.stderr
        )
    assert result.stderr.count("[hardware_api_stub] aclrtGetDeviceCount") == 1
    assert "[aclpti] initialize dependencies" not in result.stderr

    assert "npu-compute: data-directory=" not in result.stderr
    assert list(work_directory.glob("npu-compute-*")) == []
    output_directory = unpack_report(result, work_directory)
    hardware_info = output_directory / "HardwareInfo.jsonl"
    assert hardware_info.is_file()
    assert not hardware_info.is_symlink()
    assert list(output_directory.glob("HardwareInfo*.jsonl")) == [hardware_info]

    content = hardware_info.read_text(encoding="utf-8")
    lines = content.splitlines()
    assert len(lines) == 5
    host = json.loads(lines[0])
    assert list(host) == [
        "category",
        "cpu physical count",
        "cpu logical count",
        "memory total size(MB)",
        "disk total size(GB)",
    ]
    assert host["category"] == "Host Info"
    for field in (
        "cpu physical count",
        "cpu logical count",
        "memory total size(MB)",
        "disk total size(GB)",
    ):
        assert host[field] >= 0
    expected = (
        "\n".join(
            [
                lines[0],
                '{"category":"Device Info","npu count":1,'
                '"chip info":"Ascend950PR_9599 V100","arch info":"3510"}',
                '{"category":"CPU Information","control cpu count":4,'
                '"ai cpu count":8,"ai cpu frequency(MHZ)":1500}',
                '{"category":"AI Core Information","ai core count":32,'
                '"ai cube count":16,"ai vector count":16,'
                '"ai cube frequency(MHZ)":1650,"ai vector frequency(MHZ)":1650}',
                '{"category":"Memory Information","hbm total(MB)":65536,'
                '"hbm used(MB)":16384,"hbm frequency(MHZ)":3200}',
            ]
        )
        + "\n"
    )
    assert content == expected


@pytest.mark.parametrize(
    "sections",
    [
        ("PipeUtilization",),
        ("ArithmeticUtilization",),
        ("ResourceConflictRatio",),
        ("ArithmeticUtilization", "ResourceConflictRatio"),
    ],
)
def test_cli_real_aclpti_callback_chain(tmp_path, sections):
    work_directory = tmp_path / "aclpti-chain"
    work_directory.mkdir()
    environment = os.environ.copy()
    existing_preload = environment.get("LD_PRELOAD", "")
    environment["LD_PRELOAD"] = os.pathsep.join(
        value for value in (str(HARDWARE_API_STUB), existing_preload) if value
    )
    existing_library_path = environment.get("LD_LIBRARY_PATH", "")
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(
        value for value in (str(BIN_DIR), existing_library_path) if value
    )
    environment["NPU_COMPUTE_DEBUG"] = "1"
    # The profiling stub emits task-level PMU records, without block task logs.
    environment["NPU_COMPUTE_PMU_LEVEL"] = "task"
    environment.pop("INJECTION_TEST_CALLBACK_EVENTS", None)

    command = [str(CLI)]
    for section in sections:
        command.extend(("--section", section))
    command.append(str(APP))
    result = subprocess.run(
        command,
        env=environment,
        cwd=work_directory,
        text=True,
        capture_output=True,
        timeout=60,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    assert "[aclpti] subscribe result=0" in result.stderr
    for cbid in (13, 0):
        assert f"[libnpu-compute] enabled ACL PTI callback cbid={cbid}" in result.stderr
        assert (
            f"[libnpu-compute] disabled ACL PTI callback cbid={cbid}" in result.stderr
        )
    for cbid in (9, 4):
        assert (
            f"[libnpu-compute] enabled ACL PTI callback cbid={cbid}"
            not in result.stderr
        )
        assert f"runtime callback domain=1 cbid={cbid}" not in result.stderr
    assert "runtime callback domain=1 cbid=13 site=0" in result.stderr
    assert (
        "runtime callback domain=1 cbid=13 site=1 retval=0 accepted=1" in result.stderr
    )
    assert "disable ACL PTI callback failed" not in result.stderr

    assert "npu-compute: data-directory=" not in result.stderr
    assert list(work_directory.glob("npu-compute-*")) == []
    output_directory = unpack_report(result, work_directory)
    hardware_info = output_directory / "HardwareInfo.jsonl"
    assert hardware_info.is_file()
    assert len(hardware_info.read_text(encoding="utf-8").splitlines()) == 5
    assert list(output_directory.glob("HardwareInfo*.jsonl")) == [hardware_info]

    # This uses the real injected library and PTI profiler, not CSV fixture files.
    # Checking exact section names also catches injection SectionConfig omissions.
    csv_files = sorted(output_directory.rglob("*.csv"))
    assert csv_files, result.stderr
    assert {path.stem for path in csv_files} == set(sections)
    section_metrics = {
        "PipeUtilization": "aic_cube_ratio",
        "ArithmeticUtilization": "aic_cube_total_instr_number",
        "ResourceConflictRatio": "aiv_vec_sfu_cflt_ratio",
    }
    for path in csv_files:
        with path.open(encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(stream)
            rows = list(reader)
            assert section_metrics[path.stem] in reader.fieldnames
        assert rows, path
        assert all(None not in row and None not in row.values() for row in rows)
        assert all(row["block_id"] != "" and row["sub_block_id"] != "" for row in rows)

    reports = list(work_directory.glob("*.npu-rep"))
    assert len(reports) == 1, result.stderr
    imported = subprocess.run(
        [str(CLI), "--import", str(reports[0]), "--export", str(tmp_path)],
        cwd=work_directory,
        env=environment,
        text=True,
        capture_output=True,
        timeout=60,
        check=False,
    )
    assert imported.returncode == 0, imported.stderr
    prefix = "npu-compute: unpacked= "
    unpacked = [
        Path(line[len(prefix) :])
        for line in imported.stderr.splitlines()
        if line.startswith(prefix)
    ]
    assert len(unpacked) == 1, imported.stderr
    original_csv = {
        path.relative_to(output_directory): path.read_bytes() for path in csv_files
    }
    restored_csv = {
        path.relative_to(unpacked[0]): path.read_bytes()
        for path in unpacked[0].rglob("*.csv")
    }
    assert restored_csv == original_csv

    summaries = sorted(output_directory.rglob("summary.jsonl"))
    assert summaries, result.stderr
    for path in summaries:
        records = [
            json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
        ]
        assert [record["category"] for record in records] == [
            *sorted(set(sections)),
            "OpInfoSummary",
        ]
        for section, record in zip(sorted(set(sections)), records):
            assert section_metrics[section] in record
            assert "block_id" not in record
        summary = records[-1]
        assert len(summary) == 20
        assert all(value is not None for record in records for value in record.values())
        for field in (
            "aic_flops_theoretical",
            "aiv_flops_theoretical",
            "aic_flops",
            "aiv_flops",
        ):
            assert field in summary
            assert summary[field] == "NA" or isinstance(summary[field], (int, float))
        restored = unpacked[0] / path.relative_to(output_directory)
        assert restored.read_bytes() == path.read_bytes()
