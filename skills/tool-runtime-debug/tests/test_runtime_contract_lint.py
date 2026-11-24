# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import errno
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[1]))

from scripts import runtime_contract_lint as lint


def check(tmp_path: Path, body: str):
    source = tmp_path / "case.cpp"
    source.write_text(
        "#include <acl/acl.h>\nint main() {\n" + body + "\nreturn 0;\n}\n",
        encoding="utf-8",
    )
    return lint.lint_source(source)


def rules(result):
    return [item["rule"] for item in result["errors"] + result["warnings"]]


def test_null_query_output_is_definite_error_with_contract(tmp_path):
    result = check(tmp_path, "aclError rc = aclrtGetDevice(nullptr);")
    assert result["valid"] is False
    assert result["errors"][0]["rule"] == "required-output-is-null"
    assert result["errors"][0]["repair_contract"]["output_type"] == "int32_t*"


def test_query_output_type_mismatch_is_rejected(tmp_path):
    result = check(tmp_path, "int32_t device = -1; aclrtGetDevice(&device);")
    assert result["valid"] is True, result
    result = check(tmp_path, "uint32_t device = 0; aclrtGetDevice(&device);")
    assert result["valid"] is False
    assert "output-binding-type-mismatch" in rules(result)


def test_platform_alias_int_is_accepted_for_int32_output(tmp_path):
    result = check(tmp_path, "int device = -1; aclrtGetDevice(&device);")
    assert result["valid"] is True, result


def test_missing_outputs_are_reported_independently(tmp_path):
    result = check(
        tmp_path,
        "size_t free_bytes = 0; aclrtGetMemInfo(ACL_HBM_MEM, nullptr, &free_bytes);",
    )
    assert result["valid"] is False
    assert result["errors"][0]["rule"] == "required-output-is-null"
    assert result["errors"][0]["repair_contract"]["output_role"] == "free_bytes"


def test_allocator_pairing_is_checked(tmp_path):
    result = check(
        tmp_path,
        "void* host = nullptr; aclrtMallocHost(&host, 64); aclrtFree(host);",
    )
    assert result["valid"] is False
    assert result["errors"][0]["rule"] == "allocator-deallocator-mismatch"


def test_matching_allocator_pair_is_accepted(tmp_path):
    result = check(
        tmp_path,
        "void* host = nullptr; aclrtMallocHost(&host, 64); aclrtFreeHost(host);",
    )
    assert result["valid"] is True, result


def test_mixed_cleanup_is_warning_not_definite_error(tmp_path):
    result = check(
        tmp_path,
        "void* host = nullptr; bool use_host = true; "
        "aclrtMallocHost(&host, 64); "
        "use_host ? aclrtFreeHost(host) : aclrtFree(host);",
    )
    assert result["valid"] is True, result
    assert result["warnings"][0]["rule"] == "mixed-cleanup-families-require-path-check"


def test_event_must_be_recorded_before_wait(tmp_path):
    result = check(
        tmp_path,
        "aclrtEvent event = nullptr; aclrtCreateEvent(&event); "
        "aclrtSynchronizeEvent(event); aclrtDestroyEvent(event);",
    )
    assert result["valid"] is False
    assert result["errors"][0]["rule"] == "event-synchronized-before-record"
    assert "repair_contract" in result["errors"][0]


def test_event_record_then_wait_is_accepted(tmp_path):
    result = check(
        tmp_path,
        "aclrtStream stream = nullptr; aclrtEvent event = nullptr; "
        "aclrtCreateStream(&stream); aclrtCreateEvent(&event); "
        "aclrtRecordEvent(event, stream); aclrtSynchronizeEvent(event); "
        "aclrtDestroyEvent(event); aclrtDestroyStream(stream);",
    )
    assert result["valid"] is True, result


def test_zero_event_timeout_is_poll_warning(tmp_path):
    result = check(
        tmp_path,
        "aclrtStream stream = nullptr; aclrtEvent event = nullptr; "
        "aclrtCreateStream(&stream); aclrtCreateEvent(&event); "
        "aclrtRecordEvent(event, stream); "
        "aclrtSynchronizeEventWithTimeout(event, 0);",
    )
    assert result["valid"] is True, result
    assert result["warnings"][0]["rule"] == "event-zero-timeout-is-poll"


def test_zero_timeout_cannot_gate_dependent_runtime_work(tmp_path):
    result = check(
        tmp_path,
        "aclrtStream stream = nullptr; aclrtEvent event = nullptr; "
        "aclrtCreateStream(&stream); aclrtCreateEvent(&event); "
        "aclrtRecordEvent(event, stream); "
        "const aclError wait_rc = aclrtSynchronizeEventWithTimeout(event, 0); "
        "int32_t device = -1; "
        "const aclError rc = wait_rc == ACL_SUCCESS "
        "? aclrtGetDevice(&device) : wait_rc;",
    )
    assert result["valid"] is False
    assert "event-zero-timeout-gates-dependent-work" in rules(result)


def test_literal_copy_capacity_and_allocation_are_checked(tmp_path):
    result = check(
        tmp_path,
        "void* device = nullptr; void* host = nullptr; "
        "aclrtMalloc(&device, 32, ACL_MEM_MALLOC_HUGE_FIRST); "
        "aclrtMallocHost(&host, 64); "
        "aclrtMemcpy(device, 32, host, 64, ACL_MEMCPY_HOST_TO_DEVICE); "
        "aclrtFree(device); aclrtFreeHost(host);",
    )
    assert result["valid"] is False
    assert [item["rule"] for item in result["errors"]] == [
        "copy-exceeds-destination-capacity"
    ]
    assert "copy-exceeds-allocation" not in rules(result)


def test_lint_reports_only_the_earliest_definite_error(tmp_path):
    result = check(
        tmp_path,
        "void* device = nullptr; void* host = nullptr;\n"
        "aclrtMalloc(&device, 32, ACL_MEM_MALLOC_HUGE_FIRST);\n"
        "aclrtMallocHost(&host, 64);\n"
        "aclrtMemcpy(device, 32, host, -1, ACL_MEMCPY_HOST_TO_DEVICE);\n"
        "aclrtMemcpy(device, 32, host, 64, ACL_MEMCPY_HOST_TO_DEVICE);\n"
        "aclrtFree(device); aclrtFreeHost(host);",
    )
    assert result["valid"] is False
    assert len(result["errors"]) == 1
    assert result["errors"][0]["rule"] == "negative-byte-count"
    assert result["first_error"]["rule"] == "negative-byte-count"
    assert result["next_action"] == "repair_current_first_error_then_rerun_lint"
    legacy_action = "repair_" + "all_errors_then_rerun_lint"
    assert legacy_action not in json.dumps(result)


def test_zero_count_nonempty_operation_is_error(tmp_path):
    result = check(
        tmp_path,
        "void* device = nullptr; void* host = nullptr; "
        "aclrtMalloc(&device, 32, ACL_MEM_MALLOC_HUGE_FIRST); "
        "aclrtMallocHost(&host, 32); "
        "aclrtMemcpy(device, 32, host, 0, ACL_MEMCPY_HOST_TO_DEVICE); "
        "aclrtFree(device); aclrtFreeHost(host);",
    )
    assert result["valid"] is False
    assert "zero-count-nonempty-operation" in rules(result)


def test_negative_byte_count_is_error(tmp_path):
    result = check(
        tmp_path,
        "void* device = nullptr; void* host = nullptr; "
        "aclrtMalloc(&device, 32, ACL_MEM_MALLOC_HUGE_FIRST); "
        "aclrtMallocHost(&host, 32); "
        "aclrtMemcpy(device, 32, host, -1, ACL_MEMCPY_HOST_TO_DEVICE); "
        "aclrtFree(device); aclrtFreeHost(host);",
    )
    assert result["valid"] is False
    assert "negative-byte-count" in rules(result)


def test_memset_value_is_byte_checked(tmp_path):
    result = check(
        tmp_path,
        "void* device = nullptr; aclrtMalloc(&device, 32, ACL_MEM_MALLOC_HUGE_FIRST); "
        "aclrtMemset(device, 32, 0x1234, 32); aclrtFree(device);",
    )
    assert result["valid"] is False
    assert "memset-multibyte-literal" in rules(result)


def test_memset_count_must_fit_capacity(tmp_path):
    result = check(
        tmp_path,
        "void* device = nullptr; aclrtMalloc(&device, 64, ACL_MEM_MALLOC_HUGE_FIRST); "
        "aclrtMemset(device, 16, 0, 32); aclrtFree(device);",
    )
    assert result["valid"] is False
    assert result["errors"][0]["rule"] == "copy-exceeds-destination-capacity"
    assert "copy-width-exceeds-pitch" not in rules(result)


def test_unknown_event_macro_is_rejected(tmp_path):
    result = check(
        tmp_path,
        "aclrtEvent event = nullptr; "
        "aclrtCreateEventWithFlag(&event, ACL_EVENT_FLAG_TIMELINE);",
    )
    assert result["valid"] is False
    assert result["errors"][0]["rule"] == "unknown-acl-event-macro"


def test_rejects_directory_and_unsupported_suffix(tmp_path):
    directory = tmp_path / "source.cpp"
    directory.mkdir()
    with pytest.raises(ValueError, match="regular file"):
        lint.lint_source(directory)

    unsupported = tmp_path / "source.txt"
    unsupported.write_text("sentinel source body", encoding="utf-8")
    with pytest.raises(ValueError, match="unsupported source suffix"):
        lint.lint_source(unsupported)


def test_rejects_symlink_source(tmp_path):
    target = tmp_path / "target.cpp"
    target.write_text("int main() { return 0; }", encoding="utf-8")
    source = tmp_path / "source.cpp"
    source.symlink_to(target)

    with pytest.raises(ValueError, match="symbolic link"):
        lint.lint_source(source)


def test_rejects_oversized_source_before_parse(tmp_path, monkeypatch):
    source = tmp_path / "large.cpp"
    source.write_bytes(b" " * (lint.MAX_SOURCE_BYTES + 1))

    def unexpected_parse(_text):
        raise AssertionError("oversized source must be rejected before parsing")

    monkeypatch.setattr(lint, "_strip_comments", unexpected_parse)
    with pytest.raises(ValueError, match="exceeds"):
        lint.lint_source(source)


def test_cli_missing_source_uses_argparse_stderr_and_two():
    result = subprocess.run(
        [sys.executable, str(Path(lint.__file__))],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 2
    assert result.stdout == ""
    assert "usage:" in result.stderr
    assert "a source path is required" in result.stderr


@pytest.mark.parametrize("output_format", ["json", "text"])
def test_cli_boundary_error_is_json_on_stdout_for_both_formats(tmp_path, output_format):
    source = tmp_path / "invalid.txt"
    source.write_text("private sentinel source body", encoding="utf-8")
    result = subprocess.run(
        [
            sys.executable,
            str(Path(lint.__file__)),
            "--format",
            output_format,
            str(source),
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 2
    assert result.stderr == ""
    assert len(result.stdout.splitlines()) == 1
    payload = json.loads(result.stdout)
    assert payload["schema_version"] == lint.SCHEMA_VERSION
    assert payload["valid"] is False
    assert "error" in payload
    assert "private sentinel source body" not in result.stdout


def test_atomic_open_rejects_lint_path_swapped_to_symlink(tmp_path, monkeypatch):
    source = tmp_path / "source.cpp"
    source.write_text("int main() { return 0; }", encoding="utf-8")
    sentinel = tmp_path / "outside.cpp"
    sentinel.write_bytes(b"unique-lint-sentinel-bytes")
    sentinel_info = sentinel.stat()
    replacement = tmp_path / "replacement-link"
    real_open = os.open
    real_read = os.read
    real_fstat = os.fstat
    read_identities = []
    open_flags = []

    def swap_then_open(path, flags, *args, **kwargs):
        assert Path(path) == source
        open_flags.append(flags)
        replacement.symlink_to(sentinel)
        os.replace(replacement, source)
        return real_open(path, flags, *args, **kwargs)

    def track_read(fd, count):
        info = real_fstat(fd)
        read_identities.append((info.st_dev, info.st_ino))
        return real_read(fd, count)

    monkeypatch.setattr(lint.os, "open", swap_then_open)
    monkeypatch.setattr(lint.os, "read", track_read)

    with pytest.raises((OSError, ValueError)):
        lint._read_source(source)

    assert open_flags and open_flags[0] & os.O_NOFOLLOW
    assert (sentinel_info.st_dev, sentinel_info.st_ino) not in read_identities


def test_streaming_read_rejects_lint_growth_past_limit(tmp_path, monkeypatch):
    source = tmp_path / "growing.cpp"
    source.write_bytes(b"a")
    limit = 31
    monkeypatch.setattr(lint, "MAX_SOURCE_BYTES", limit)
    mutation_fd = os.open(source, os.O_WRONLY | os.O_APPEND)
    real_read = os.read
    returned = []
    growth = b"unique-growth-payload-" + b"x" * 48
    calls = 0

    def grow_after_first_read(fd, count):
        nonlocal calls
        chunk = real_read(fd, count)
        returned.append(chunk)
        calls += 1
        if calls == 1:
            os.write(mutation_fd, growth)
        return chunk

    try:
        monkeypatch.setattr(lint.os, "read", grow_after_first_read)
        with pytest.raises(ValueError, match="exceeds") as error:
            lint._read_source(source)
    finally:
        os.close(mutation_fd)

    assert sum(map(len, returned)) <= limit + 1
    assert growth.decode("ascii") not in str(error.value)


def test_streaming_read_rejects_lint_shrink_during_read(tmp_path, monkeypatch):
    source = tmp_path / "shrinking.cpp"
    source.write_bytes(b"secret-source-body-" + b"a" * (128 * 1024))
    mutation_fd = os.open(source, os.O_WRONLY)
    real_read = os.read
    returned = []
    calls = 0

    def shrink_after_first_read(fd, count):
        nonlocal calls
        chunk = real_read(fd, count)
        returned.append(chunk)
        calls += 1
        if calls == 1:
            os.ftruncate(mutation_fd, 8)
        return chunk

    try:
        monkeypatch.setattr(lint.os, "read", shrink_after_first_read)
        with pytest.raises(ValueError, match="^file changed while reading$") as error:
            lint._read_source(source)
    finally:
        os.close(mutation_fd)

    assert len(returned[0]) == 64 * 1024
    assert "secret-source-body" not in str(error.value)


def test_lint_closes_owned_fd_on_validation_or_read_error(tmp_path, monkeypatch):
    source = tmp_path / "source.cpp"
    source.write_text("int main() { return 0; }", encoding="utf-8")
    real_open = os.open
    real_close = os.close
    real_fstat = os.fstat

    for failure_stage in ("validation", "read"):
        opened = []
        closed = []

        def record_open(path, flags, *args, **kwargs):
            fd = real_open(path, flags, *args, **kwargs)
            opened.append(fd)
            return fd

        def record_close(fd):
            closed.append(fd)
            return real_close(fd)

        with monkeypatch.context() as scoped:
            scoped.setattr(lint.os, "open", record_open)
            scoped.setattr(lint.os, "close", record_close)
            if failure_stage == "validation":

                def fail_fstat(fd):
                    if fd in opened:
                        raise RuntimeError("injected validation failure")
                    return real_fstat(fd)

                scoped.setattr(lint.os, "fstat", fail_fstat)
                expected_message = "injected validation failure"
            else:

                def fail_read(_fd, _count):
                    raise RuntimeError("injected read failure")

                scoped.setattr(lint.os, "read", fail_read)
                expected_message = "injected read failure"

            with pytest.raises(RuntimeError, match=expected_message):
                lint._read_source(source)

        assert len(opened) == 1
        assert closed.count(opened[0]) == 1
        with pytest.raises(OSError) as error:
            os.fstat(opened[0])
        assert error.value.errno == errno.EBADF


def test_secure_open_fails_closed_without_nofollow(tmp_path, monkeypatch):
    source = tmp_path / "source.cpp"
    source.write_text("int main() { return 0; }", encoding="utf-8")
    real_open = os.open
    real_read = os.read
    open_calls = []
    read_calls = []

    def record_open(*args, **kwargs):
        open_calls.append(args)
        return real_open(*args, **kwargs)

    def record_read(*args, **kwargs):
        read_calls.append(args)
        return real_read(*args, **kwargs)

    monkeypatch.delattr(lint.os, "O_NOFOLLOW")
    monkeypatch.setattr(lint.os, "open", record_open)
    monkeypatch.setattr(lint.os, "read", record_read)

    with pytest.raises(OSError) as error:
        lint._open_regular_no_follow(source, max_bytes=lint.MAX_SOURCE_BYTES)

    assert error.value.errno == errno.ENOTSUP
    assert open_calls == []
    assert read_calls == []
