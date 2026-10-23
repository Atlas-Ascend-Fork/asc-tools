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
import struct
import subprocess
from pathlib import Path

import pytest


BUILD_DIR = Path(
    os.environ.get("NPU_COMPUTE_BUILD_DIR", "/tmp/asc_tools_npu_compute_integration")
)
BIN_DIR = Path(os.environ.get("NPU_COMPUTE_TEST_BIN_DIR", str(BUILD_DIR / "bin")))
CLI = BIN_DIR / "npu-compute"
SECTIONS = [
    "PipeUtilization",
    "Memory",
    "MemoryL0",
    "MemoryUB",
    "L2Cache",
    "ArithmeticUtilization",
    "ResourceConflictRatio",
]
FIXTURE = Path(
    os.environ.get(
        "NPU_COMPUTE_REP_FIXTURE", str(BIN_DIR / "npu_compute_rep_fixture_app")
    )
)
BASIC_SECTIONS = ["Pipeline", *SECTIONS[:-1]]


def fixture_command(*arguments):
    assert FIXTURE.is_file(), f"report fixture was not built: {FIXTURE}"
    return [str(FIXTURE), "--mode", "cli-echo", "--", *arguments]


def received_arguments(result):
    return [
        bytes.fromhex(line.removeprefix("argument=")).decode()
        for line in result.stdout.splitlines()
        if line.startswith("argument=")
    ]


def received_config(result):
    return dict(
        line.split("=", 1)
        for line in result.stdout.splitlines()
        if line.startswith(("sections=", "replay="))
    )


def run_cli(*arguments, cwd=None):
    assert CLI.is_file(), f"npu-compute was not built: {CLI}"
    return subprocess.run(
        [str(CLI), *arguments],
        cwd=cwd,
        text=True,
        capture_output=True,
        check=False,
    )


def run_cli_with_combined_output(*arguments, cwd=None):
    assert CLI.is_file(), f"npu-compute was not built: {CLI}"
    return subprocess.run(
        [str(CLI), *arguments],
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def test_help_lists_only_the_public_command_line_options():
    result = run_cli("--help")

    assert result.returncode == 0
    assert "npu-compute [options] [--] [program] [program-arguments]" in result.stdout
    assert "Optional '--' separates" not in result.stdout
    assert (
        result.stdout.count(
            "Optional separator between npu-compute options and the program."
        )
        == 1
    )
    assert "Required if the program name starts with '-'." in result.stdout
    for option in (
        "-h, --help",
        "--section",
        "--list-sections",
        "--replay-mode",
        "-i, --import",
        "-o, --export",
    ):
        assert option in result.stdout
    for obsolete_option in ("--target", "--session", "--metric", "--dry-run"):
        assert obsolete_option not in result.stdout


@pytest.mark.parametrize(
    "arguments",
    [
        ("-h",),
        ("--help",),
        ("-h", "--help"),
        ("--section", "Memory", "--help", "--export", "result.npu-rep"),
    ],
)
def test_help_with_valid_tool_options_exits_zero(arguments, tmp_path):
    result = run_cli(*arguments, cwd=tmp_path)

    assert result.returncode == 0
    assert result.stdout.count("Usage:") == 1
    assert result.stderr == ""
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize(
    ("arguments", "expected_errors"),
    [
        (
            ("--bad-option", "--help"),
            ["unknown option '--bad-option'. Use --help to see supported options."],
        ),
        (
            ("--section", "--help"),
            [
                "--section requires a section name. Use --list-sections to see supported names."
            ],
        ),
        (
            ("--bad-one", "--bad-two", "--help"),
            [
                "unknown option '--bad-one'. Use --help to see supported options.",
                "unknown option '--bad-two'. Use --help to see supported options.",
            ],
        ),
        (
            ("--bad-option", "--list-sections", "--help"),
            ["unknown option '--bad-option'. Use --help to see supported options."],
        ),
        (
            (
                "--section",
                "Invalid",
                "--replay-mode",
                "invalid",
                "--help",
            ),
            [
                "unsupported section name 'Invalid'. Names are case-sensitive; use --list-sections to see supported names.",
                "unsupported replay mode 'invalid'. Supported value: kernel.",
            ],
        ),
    ],
)
def test_help_reports_all_option_errors(arguments, expected_errors, tmp_path):
    result = run_cli(*arguments, cwd=tmp_path)

    assert result.returncode == 2
    assert result.stdout.count("Usage:") == 1
    assert result.stderr.splitlines() == [
        f"[ERROR] npu-compute: {message}" for message in expected_errors
    ]
    assert list(tmp_path.iterdir()) == []


def test_help_prints_all_errors_before_usage_and_does_not_run_program(tmp_path):
    marker = tmp_path / "target-ran"
    result = run_cli_with_combined_output(
        "--bad-one",
        "--bad-two",
        "--help",
        "/bin/sh",
        "-c",
        f"touch {marker}",
        cwd=tmp_path,
    )

    assert result.returncode == 2
    assert result.stdout.count("Usage:") == 1
    assert result.stdout.index(
        "unknown option '--bad-one'. Use --help to see supported options."
    ) < result.stdout.index(
        "unknown option '--bad-two'. Use --help to see supported options."
    )
    assert result.stdout.index(
        "unknown option '--bad-two'. Use --help to see supported options."
    ) < result.stdout.index("Usage:")
    assert not marker.exists()
    assert list(tmp_path.iterdir()) == []


def test_default_basic_is_sent_to_application(tmp_path):
    result = run_cli(*fixture_command(), cwd=tmp_path)
    assert result.returncode == 0, result.stderr
    assert received_config(result) == {
        "sections": ",".join(BASIC_SECTIONS),
        "replay": "kernel",
    }


def test_list_sections_outputs_only_ids_in_alphabetical_order():
    result = run_cli("--list-sections")

    assert result.returncode == 0
    assert result.stdout.splitlines() == sorted(
        [*SECTIONS, "Pipeline", "PmSampling"], key=str.casefold
    )
    assert result.stderr == ""


@pytest.mark.parametrize(
    "arguments",
    [
        ("--list-sections", "--help"),
        ("--help", "--list-sections"),
    ],
)
def test_help_takes_precedence_over_list_sections(arguments, tmp_path):
    result = run_cli(*arguments, cwd=tmp_path)

    assert result.returncode == 0
    assert result.stdout.count("Usage:") == 1
    assert result.stderr == ""
    assert not any(line in SECTIONS for line in result.stdout.splitlines())
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize("section", SECTIONS)
def test_each_supported_section_is_accepted(section, tmp_path):
    result = run_cli("--section", section, *fixture_command(), cwd=tmp_path)
    assert result.returncode == 0, result.stderr
    assert received_config(result)["sections"] == section


@pytest.mark.parametrize("abbreviation", ("--l", "--list"))
def test_long_option_abbreviations_are_rejected(abbreviation):
    result = run_cli(abbreviation)

    assert result.returncode == 2
    assert result.stdout == ""
    assert "unknown option" in result.stderr


def test_collection_deduplicates_sections_and_sets_default_replay_mode(tmp_path):
    result = run_cli(
        "--section",
        "Memory",
        "--section",
        "Memory",
        "--section",
        "L2Cache",
        *fixture_command(),
        cwd=tmp_path,
    )
    assert result.returncode == 0, result.stderr
    assert received_config(result) == {"sections": "Memory,L2Cache", "replay": "kernel"}


def test_arguments_after_program_are_passed_to_the_app_verbatim(tmp_path):
    arguments = [
        "--section",
        "app-owned-value",
        "",
        "two words",
        "",
        "line\nbreak",
        "--help=x",
    ]
    result = run_cli("--section", "Memory", *fixture_command(*arguments), cwd=tmp_path)
    assert result.returncode == 0, result.stderr
    assert received_arguments(result) == arguments


@pytest.mark.parametrize("app_argument", ("-h", "--help"))
def test_help_after_program_is_passed_to_the_app(app_argument, tmp_path):
    result = run_cli(
        "--section", "Memory", *fixture_command(app_argument), cwd=tmp_path
    )
    assert result.returncode == 0, result.stderr
    assert received_arguments(result) == [app_argument]


@pytest.mark.parametrize(
    "arguments",
    [
        (),
        ("--section", "Memory"),
        ("--section", "Unknown", "/bin/true"),
        ("--section", "HardwareInfo", "/bin/true"),
        ("--section", "Memory", "--replay-mode", "application", "/bin/true"),
        (
            "--section",
            "Memory",
            "--replay-mode",
            "kernel",
            "--replay-mode",
            "kernel",
            "/bin/true",
        ),
        ("--section", "Memory", "--"),
        ("--help=value",),
        ("-hh",),
        ("--list-sections", "--section", "Memory"),
        ("--export", "result.repo"),
        ("--import", "one.repo", "--import", "two.repo"),
        ("--export", "one.repo", "--export", "two.repo"),
    ],
)
def test_invalid_options_and_combinations_exit_two(arguments):
    result = run_cli(*arguments)

    assert result.returncode == 2
    assert result.stderr.startswith("[ERROR] npu-compute:")


@pytest.mark.parametrize(
    "arguments",
    [
        ("--import", "missing.npu-rep"),
        ("--import", "missing.npu-rep", "--export", "restored"),
    ],
)
def test_missing_import_report_returns_report_error(arguments, tmp_path):
    result = run_cli(*arguments, cwd=tmp_path)

    assert result.returncode == 4
    assert "does not exist" in result.stderr
    assert "--import expects a report file, but " in result.stderr
    assert list(tmp_path.iterdir()) == []


def test_collection_rejects_export_path_without_report_suffix(tmp_path):
    result = run_cli(
        "--section",
        "Memory",
        "--export",
        "output.repo",
        "/bin/true",
        cwd=tmp_path,
    )

    assert result.returncode == 4
    assert (
        "--export expects a new .npu-rep file or an existing directory" in result.stderr
    )


@pytest.mark.parametrize(
    "obsolete_option",
    ("--target", "--session", "--metric", "--dry-run"),
)
def test_pr_prototype_options_are_rejected(obsolete_option):
    result = run_cli(obsolete_option)

    assert result.returncode == 2
    assert "unknown option" in result.stderr


@pytest.mark.parametrize("option", ["--replay-mode", "--import", "--export"])
@pytest.mark.parametrize("tail", [[], ["kernel"], ["invalid"], [""], ["--help"]])
def test_missing_value_records_option_occurrence(option, tail):
    missing = {
        "--replay-mode": "--replay-mode requires a mode. Supported value: kernel.",
        "--import": "--import expects a report file, but no file was provided.",
        "--export": "--export requires an output path: a report file or directory.",
    }[option]
    result = run_cli(option, "--help", option, *tail)
    second = (
        missing
        if not tail or tail == ["--help"]
        else option + " may only be specified once"
    )
    assert result.returncode == 2
    assert result.stderr.splitlines() == [
        "[ERROR] npu-compute: " + missing,
        "[ERROR] npu-compute: " + second,
    ]
    assert "Usage:" in result.stdout


def test_replay_missing_value_preserves_other_error_order():
    result = run_cli(
        "--bad",
        "--replay-mode",
        "--help",
        "--section",
        "Invalid",
        "--replay-mode",
        "invalid",
    )
    assert result.returncode == 2
    assert result.stderr.splitlines() == [
        "[ERROR] npu-compute: unknown option '--bad'. Use --help to see supported options.",
        "[ERROR] npu-compute: --replay-mode requires a mode. Supported value: kernel.",
        "[ERROR] npu-compute: unsupported section name 'Invalid'. Names are case-sensitive; use --list-sections to see supported names.",
        "[ERROR] npu-compute: --replay-mode may only be specified once",
    ]


@pytest.fixture
def argv_app(tmp_path, monkeypatch):
    def create(name):
        app = tmp_path / name
        assert FIXTURE.is_file()
        app.symlink_to(FIXTURE)
        return name

    monkeypatch.setenv("PATH", str(tmp_path) + os.pathsep + os.environ.get("PATH", ""))
    return create


@pytest.mark.parametrize("program", ("--app", "-app", "--help", "-h", "--"))
def test_separator_launches_dash_named_program(program, argv_app, tmp_path):
    app = argv_app(program)
    arguments = [
        "--help",
        "-h",
        "--export",
        "app-owned",
        "--section",
        "Pipeline",
        "--",
        "",
        "two words",
    ]
    result = run_cli(
        "--section",
        "Memory",
        "--",
        app,
        "--mode",
        "cli-echo",
        "--",
        *arguments,
        cwd=tmp_path,
    )

    assert result.returncode == 0, result.stderr
    assert "Usage:" not in result.stdout
    assert received_arguments(result) == arguments
    assert not (tmp_path / "app-owned").exists()


@pytest.mark.parametrize("separator", ((), ("--",)))
def test_optional_separator_preserves_application_arguments(
    separator, argv_app, tmp_path
):
    app = argv_app("argv-app")
    arguments = ["--", "--help", "--export", "app-owned", "", "two words"]
    result = run_cli(
        "--section",
        "Memory",
        *separator,
        app,
        "--mode",
        "cli-echo",
        "--",
        *arguments,
        cwd=tmp_path,
    )

    assert result.returncode == 0, result.stderr
    assert received_arguments(result) == arguments


@pytest.mark.parametrize(
    ("options", "error"),
    [
        (("--section",), "--section requires a section name."),
        (("--replay-mode",), "--replay-mode requires a mode."),
        (("--import",), "--import expects a report file, but no file was provided."),
        (("-i",), "--import expects a report file, but no file was provided."),
        (("--export",), "--export requires an output path:"),
        (("-o",), "--export requires an output path:"),
        (
            ("--import", "missing.npu-rep"),
            "Use --import alone or with --export as the only additional option.",
        ),
        (("--list-sections",), "use --list-sections as a standalone command."),
        (("--bad",), "unknown option '--bad'"),
    ],
)
def test_separator_errors_do_not_launch_application(options, error, argv_app, tmp_path):
    app = argv_app("--app")
    result = run_cli(*options, "--", app, "--help", cwd=tmp_path)

    assert result.returncode == 2
    assert error in result.stderr
    assert "sections=" not in result.stdout
    assert list(tmp_path.iterdir()) == [tmp_path / "--app"]


@pytest.mark.parametrize("option", ["--help", "--list-sets", "--list-sections"])
@pytest.mark.parametrize("value", ["", "app"])
def test_flag_value_errors_are_specific_and_do_not_launch(option, value, tmp_path):
    result = run_cli(option + "=" + value, *fixture_command(), cwd=tmp_path)
    assert result.returncode == 2
    assert result.stdout == ""
    assert result.stderr == f"[ERROR] npu-compute: {option} does not take a value.\n"
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize(
    ("arguments", "message"),
    [
        (("--import",), "--import expects a report file, but no file was provided."),
        (
            ("--import=",),
            "--import expects a report file, but no file was specified.",
        ),
        (
            ("--import", ""),
            "--import expects a report file, but no file was specified.",
        ),
    ],
)
def test_import_argument_errors(arguments, message, tmp_path):
    result = run_cli(*arguments, cwd=tmp_path)
    assert result.returncode == 2
    assert result.stdout == ""
    assert result.stderr == f"[ERROR] npu-compute: {message}\n"
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize("option", ["-i", "--import"])
@pytest.mark.parametrize("kind", ["missing", "directory", "empty", "corrupt"])
def test_invalid_import_preserves_reason_and_creates_no_output(option, kind, tmp_path):
    file = tmp_path / "app"
    if kind == "directory":
        file.mkdir()
    elif kind == "empty":
        file.write_bytes(b"")
    elif kind == "corrupt":
        file.write_bytes(b"invalid" * 32)
    before = set(tmp_path.iterdir())
    result = run_cli(option, "app", cwd=tmp_path)
    assert result.returncode == 4
    assert result.stdout == ""
    if kind in ("missing", "directory"):
        assert result.stderr.startswith(
            "[ERROR] npu-compute: --import expects a report file, but 'app' "
        )
    else:
        assert result.stderr.startswith(
            "[ERROR] npu-compute: invalid npu-compute report 'app': "
        )
        assert "--import expects" not in result.stderr
    assert "Invalid input" not in result.stderr
    assert "regular file" not in result.stderr
    reason = {
        "missing": "does not exist",
        "directory": "directory",
        "empty": "invalid file format: the file is empty",
        "corrupt": "invalid file format",
    }[kind]
    assert reason in result.stderr
    assert "file path" not in result.stderr
    assert set(tmp_path.iterdir()) == before


def test_help_does_not_read_import_file(tmp_path):
    result = run_cli("--import", "app", "--help", cwd=tmp_path)
    assert result.returncode == 0
    assert result.stderr == ""
    assert result.stdout.count("Usage:") == 1
    assert list(tmp_path.iterdir()) == []


def test_help_has_one_definition_per_option():
    result = run_cli("--help")
    assert result.returncode == 0
    definitions = []
    for line in result.stdout.splitlines():
        if len(line) - len(line.lstrip()) not in (2, 6):
            continue
        words = line.split()
        if words:
            definitions.append(words[1] if words[0].endswith(",") else words[0])
    for option in [
        "--help",
        "--list-sets",
        "--list-sections",
        "--set",
        "--section",
        "--replay-mode",
        "--import",
        "--export",
    ]:
        assert definitions.count(option) == 1
    assert "No section is selected by default" not in result.stdout
    assert "file path" not in result.stdout


@pytest.mark.parametrize(
    ("options", "sections"),
    [
        (("--set", "basic"), BASIC_SECTIONS),
        (("--set", "full"), [*BASIC_SECTIONS, "ResourceConflictRatio", "PmSampling"]),
        (
            (
                "--section",
                "Memory",
                "--set",
                "basic",
                "--set",
                "basic",
                "--section",
                "Memory",
            ),
            ["Memory", *[name for name in BASIC_SECTIONS if name != "Memory"]],
        ),
    ],
)
def test_set_expansion_and_first_occurrence_order(options, sections, tmp_path):
    result = run_cli(*options, *fixture_command(), cwd=tmp_path)
    assert result.returncode == 0, result.stderr
    assert received_config(result)["sections"] == ",".join(sections)


@pytest.mark.parametrize(
    "options",
    [("--list-sets", "--section", "Memory"), ("--import", "app", "--set", "basic")],
)
def test_invalid_modes_do_not_launch(options, tmp_path):
    result = run_cli(*options, *fixture_command(), cwd=tmp_path)
    assert result.returncode == 2
    assert result.stdout == ""
    assert list(tmp_path.iterdir()) == []


def test_collected_report_imports_and_unsupported_name_leaves_no_output(tmp_path):
    input_file = tmp_path / "collected.npu-rep"
    result = run_cli("--export", str(input_file), *fixture_command(), cwd=tmp_path)
    assert result.returncode == 0, result.stderr
    output = tmp_path / "restored"
    output.mkdir()
    imported = run_cli(
        "--import", str(input_file), "--export", str(output), cwd=tmp_path
    )
    assert imported.returncode == 0, imported.stderr
    assert imported.stdout == ""
    children = list(output.iterdir())
    assert len(children) == 1
    assert (children[0] / "HardwareInfo.jsonl").is_file()
    assert (children[0] / "Memory.csv").is_file()
    renamed = tmp_path / "app"
    renamed.write_bytes(input_file.read_bytes())
    before = set(tmp_path.iterdir())
    invalid = run_cli("--import", "app", cwd=tmp_path)
    assert invalid.returncode == 4
    assert "unsupported report file name" in invalid.stderr
    assert "--import expects a report file, but " in invalid.stderr
    assert set(tmp_path.iterdir()) == before


@pytest.mark.parametrize("option", ["-i", "--import"])
@pytest.mark.parametrize(
    "kind",
    [
        "empty",
        "short_magic",
        "wrong_magic",
        "short_version",
        "truncated",
        "bad_length",
        "unknown_version",
        "nested",
    ],
)
def test_import_format_diagnostics(option, kind, tmp_path):
    magic = b"npu-rep\0"
    header = struct.pack("<8sIHHIIIQ", magic, 0x10000, 1, 36, 0, 160, 0, 36)
    samples = {
        "empty": (b"", "invalid file format: the file is empty"),
        "short_magic": (magic[:4], "invalid file format"),
        "wrong_magic": (b"x" + header[1:], "invalid file format"),
        "short_version": (magic, "file is corrupted"),
        "truncated": (header[:20], "file is corrupted"),
        "bad_length": (header + b"extra", "file is corrupted"),
        "unknown_version": (
            magic + struct.pack("<I", 0x20000) + header[12:],
            "unsupported report version",
        ),
    }
    # The outer report is valid, but its nested report cannot be decoded.
    payload = b"invalid"
    nested_header = struct.pack(
        "<8sIHHIIIQ", magic, 0x10000, 1, 36, 1, 160, 0, 196 + len(payload)
    )
    info = struct.pack(
        "<8s128sHHIQQ", magic, b"internal.rep", 1, 0, 0, len(payload), 196
    )
    samples["nested"] = (nested_header + info + payload, "file is corrupted")
    data, reason = samples[kind]
    file = tmp_path / "input.npu-rep"
    file.write_bytes(data)
    result = run_cli(option, file.name, cwd=tmp_path)
    assert result.returncode == 4
    assert (
        result.stderr
        == f"[ERROR] npu-compute: invalid npu-compute report '{file.name}': {reason}.\n"
    )
    assert result.stdout == ""
    assert list(tmp_path.iterdir()) == [file]
