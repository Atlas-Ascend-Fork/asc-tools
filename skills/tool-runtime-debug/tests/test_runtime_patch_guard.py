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

from scripts import runtime_patch_guard as guard


SOURCE = """#include <acl/acl.h>
#include <cstdio>
int main() {
    int device = -1;
    const aclError query_rc = aclrtGetDevice(nullptr);
    const bool verify = query_rc == ACL_SUCCESS && device == 0;
    std::printf("[VERIFY] %s device=%d\\n", verify ? "PASS" : "FAIL", device);
    return 0;
}
"""


def _supports_dir_fd_with_open(open_function, original_open):
    supported = set(os.supports_dir_fd)
    supported.discard(original_open)
    supported.add(open_function)
    return supported


def check(tmp_path: Path, candidate: str):
    return check_pair(tmp_path, SOURCE, candidate)


def check_pair(tmp_path: Path, before: str, candidate: str):
    path = tmp_path / "main.cpp"
    path.write_text(before, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    return guard.check_snapshot(snapshot, path)


def test_runtime_argument_repair_preserves_verifier(tmp_path):
    candidate = SOURCE.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    result = check(tmp_path, candidate)
    assert result["valid"] is True, result
    assert result["checks"]["runtime_semantics_changed"] is True


def test_business_sync_api_can_be_added_without_relaxing_verifier(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    aclrtStream stream = nullptr;\n"
        "    aclrtSynchronizeStream(stream);\n"
        "    const bool verify =",
    )
    result = check_pair(
        tmp_path,
        source,
        source.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)"),
    )
    assert result["valid"] is True, result
    assert result["checks"]["runtime_calls_preserved"] is True
    assert result["checks"]["immutable_verification_surface_unchanged"] is True


def test_business_release_api_can_be_added(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    void* buffer = nullptr;\n    aclrtFree(buffer);\n    const bool verify =",
    )
    result = check_pair(
        tmp_path,
        source,
        source.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)"),
    )
    assert result["valid"] is True, result


def test_wrong_release_can_be_removed_when_cleanup_remains(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    void* host = nullptr;\n"
        "    aclrtMallocHost(&host, 64);\n"
        "    aclrtFree(host);\n"
        "    aclrtFreeHost(host);\n"
        "    const bool verify =",
    )
    candidate = source.replace("    aclrtFree(host);\n", "")
    candidate = candidate.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is True, result
    assert result["checks"]["cleanup_mutations_allowed"] is True


def test_wrong_stream_can_be_replaced_by_producing_stream(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    aclrtStream wrong_stream = nullptr;\n"
        "    aclrtStream producing_stream = nullptr;\n"
        "    aclrtEvent event = nullptr;\n"
        "    aclrtRecordEvent(event, wrong_stream);\n"
        "    const bool verify =",
    )
    candidate = source.replace(
        "aclrtRecordEvent(event, wrong_stream)",
        "aclrtRecordEvent(event, producing_stream)",
    )
    result = check_pair(tmp_path, source, candidate)
    assert result["valid"] is True, result
    assert result["runtime_argument_edits"][0]["parameter_index"] == 1
    assert result["runtime_argument_edits"][0]["traceable"] is True


def test_wrong_event_can_be_replaced_by_recorded_event(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    aclrtStream stream = nullptr;\n"
        "    aclrtEvent wrong_event = nullptr;\n"
        "    aclrtEvent recorded_event = nullptr;\n"
        "    aclrtRecordEvent(wrong_event, stream);\n"
        "    const bool verify =",
    )
    candidate = source.replace(
        "aclrtRecordEvent(wrong_event, stream)",
        "aclrtRecordEvent(recorded_event, stream)",
    )
    result = check_pair(tmp_path, source, candidate)
    assert result["valid"] is True, result
    assert result["runtime_argument_edits"][0]["parameter_index"] == 0


def test_wrong_capacity_can_be_replaced_by_declared_capacity(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    void* device_buffer = nullptr;\n"
        "    void* host_buffer = nullptr;\n"
        "    size_t wrong_capacity = 32;\n"
        "    size_t correct_capacity = 64;\n"
        "    aclrtMemcpy(device_buffer, wrong_capacity, host_buffer, 16, ACL_MEMCPY_HOST_TO_DEVICE);\n"
        "    const bool verify =",
    )
    candidate = source.replace(
        "aclrtMemcpy(device_buffer, wrong_capacity, host_buffer, 16",
        "aclrtMemcpy(device_buffer, correct_capacity, host_buffer, 16",
    )
    result = check_pair(tmp_path, source, candidate)
    assert result["valid"] is True, result
    assert result["runtime_argument_edits"][0]["parameter_index"] == 1


def test_unrelated_batch_runtime_argument_replacements_are_rejected(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    aclrtStream wrong_stream = nullptr;\n"
        "    aclrtStream producing_stream = nullptr;\n"
        "    aclrtEvent wrong_event = nullptr;\n"
        "    aclrtEvent recorded_event = nullptr;\n"
        "    aclrtRecordEvent(wrong_event, wrong_stream);\n"
        "    aclrtRecordEvent(wrong_event, wrong_stream);\n"
        "    const bool verify =",
    )
    candidate = source.replace(
        "aclrtRecordEvent(wrong_event, wrong_stream);",
        "aclrtRecordEvent(recorded_event, producing_stream);",
    )
    result = check_pair(tmp_path, source, candidate)
    assert result["valid"] is False
    assert result["checks"]["runtime_argument_edit_scope"] is False


def test_comment_only_edit_is_rejected(tmp_path):
    candidate = SOURCE.replace("#include <cstdio>", "#include <cstdio>\n// note")
    result = check(tmp_path, candidate)
    assert result["valid"] is False
    assert result["checks"]["runtime_semantics_changed"] is False


def test_observation_rewrite_is_rejected(tmp_path):
    candidate = SOURCE.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    candidate = candidate.replace("device=%d", "current=%d")
    result = check(tmp_path, candidate)
    assert result["valid"] is False
    assert result["checks"]["observation_calls_unchanged"] is False


def test_hardcoded_pass_is_rejected(tmp_path):
    candidate = SOURCE.replace('verify ? "PASS" : "FAIL"', '"PASS"')
    candidate = candidate.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    result = check(tmp_path, candidate)
    assert result["valid"] is False
    assert result["checks"]["observation_calls_unchanged"] is False


def test_hardcoded_return_zero_is_rejected(tmp_path):
    source = SOURCE.replace("return 0;", "return verify ? 0 : 1;")
    candidate = source.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    candidate = candidate.replace("return verify ? 0 : 1;", "return 0;")
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["return_semantics_unchanged"] is False


def test_indirect_run_case_return_zero_is_rejected(tmp_path):
    source = """#include <acl/acl.h>
int run_case() {
    const bool matches_reference = true;
    return matches_reference ? 0 : 1;
}
int main() {
    return run_case();
}
"""
    candidate = source.replace("return matches_reference ? 0 : 1;", "return 0;")
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["return_semantics_unchanged"] is False


def test_overloaded_run_case_called_by_main_is_protected(tmp_path):
    source = """#include <acl/acl.h>
int run_case(int value) { return value == 1 ? 0 : 1; }
int run_case(double value) { return value > 0 ? 0 : 1; }
int main() { return run_case(1); }
"""
    candidate = source.replace("return value == 1 ? 0 : 1;", "return 0;")
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["return_semantics_unchanged"] is False


def test_same_named_run_case_in_namespaces_is_conservatively_protected(tmp_path):
    source = """#include <acl/acl.h>
namespace first {
int run_case() { return 1; }
}
namespace second {
int run_case() { return 1; }
}
int main() { return first::run_case(); }
"""
    candidate = source.replace(
        "int run_case() { return 1; }", "int run_case() { return 0; }", 1
    )
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["return_semantics_unchanged"] is False


def test_swapped_overload_signatures_and_actual_caller_return_zero_are_rejected(
    tmp_path,
):
    source = """#include <acl/acl.h>
int run_case(int value) { return value == 1 ? 0 : 1; }
int run_case(double value) { return value > 0 ? 0 : 1; }
int main() {
    aclrtStream wrong_stream = nullptr;
    aclrtStream producing_stream = nullptr;
    aclrtEvent event = nullptr;
    aclrtRecordEvent(event, wrong_stream);
    return run_case(1);
}
"""
    candidate = (
        source.replace(
            "int run_case(int value) { return value == 1 ? 0 : 1; }",
            "int run_case(double value) { return value == 1 ? 0 : 1; }",
        )
        .replace(
            "int run_case(double value) { return value > 0 ? 0 : 1; }",
            "int run_case(int value) { return 0; }",
        )
        .replace(
            "aclrtRecordEvent(event, wrong_stream)",
            "aclrtRecordEvent(event, producing_stream)",
        )
    )
    result = check_pair(tmp_path, source, candidate)
    assert result["valid"] is False
    assert result["checks"]["return_semantics_unchanged"] is False
    assert result["runtime_argument_edits"][0]["traceable"] is True


def test_comparison_predicate_is_protected_even_when_named_matches_reference(tmp_path):
    source = SOURCE.replace(
        "    const bool verify = query_rc == ACL_SUCCESS && device == 0;",
        "    const bool matches_reference = query_rc == ACL_SUCCESS && device == 0;\n"
        "    const bool verify = matches_reference;",
    )
    candidate = source.replace(
        "const bool matches_reference = query_rc == ACL_SUCCESS && device == 0;",
        "const bool matches_reference = true;",
    ).replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["verification_predicates_unchanged"] is False


def test_protected_exit_change_is_rejected(tmp_path):
    candidate = SOURCE.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    candidate = candidate.replace("return 0;", "return verify ? 0 : 1;")
    result = check(tmp_path, candidate)
    assert result["valid"] is False
    assert result["checks"]["return_semantics_unchanged"] is False


def test_removed_cleanup_responsibility_is_rejected(tmp_path):
    source = SOURCE.replace(
        "return 0;",
        "if (device != -1) aclrtFree(&device);\n"
        "    if (device != -1) aclrtFreeHost(&device);\n"
        "    return 0;",
    )
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    candidate = source.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    candidate = candidate.replace("    if (device != -1) aclrtFree(&device);\n", "")
    candidate = candidate.replace("    if (device != -1) aclrtFreeHost(&device);\n", "")
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["cleanup_families_preserved"] is False


def test_output_binding_must_not_invent_type(tmp_path):
    source = SOURCE.replace(
        "int device = -1;",
        "int device = -1;\n    uint32_t priority = 0;",
    ).replace("aclrtGetDevice(nullptr)", "aclrtStreamGetPriority(nullptr, nullptr)")
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    candidate = source.replace("uint32_t priority = 0;", "int32_t priority = 0;")
    candidate = candidate.replace(
        "aclrtStreamGetPriority(nullptr, nullptr)",
        "aclrtStreamGetPriority(nullptr, &priority)",
    )
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["output_binding_types_unchanged"] is False


def test_verification_predicate_hardcoding_and_deletion(tmp_path):
    repaired = SOURCE.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    predicate = "const bool verify = query_rc == ACL_SUCCESS && device == 0;"
    for replacement in (
        "const bool verify = true;",
        "",
        "const bool verify{true};",
        predicate + "\nverify = true;",
    ):
        result = check(tmp_path, repaired.replace(predicate, replacement))
        assert result["valid"] is False, replacement
        assert result["checks"]["verification_predicates_unchanged"] is False


def test_sleep_cannot_replace_a_runtime_synchronization(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    aclrtStream stream = nullptr;\n"
        "    aclrtSynchronizeStream(stream);\n"
        "    const bool verify =",
    )
    candidate = source.replace(
        "aclrtSynchronizeStream(stream);",
        "std::this_thread::sleep_for(std::chrono::milliseconds(1));",
    )
    candidate = candidate.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["sleep_calls_unchanged"] is False


def test_existing_sleep_cannot_hide_deleted_synchronization(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    aclrtStream stream = nullptr;\n"
        "    std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
        "    aclrtSynchronizeStream(stream);\n"
        "    const bool verify =",
    )
    candidate = source.replace("    aclrtSynchronizeStream(stream);\n", "")
    candidate = candidate.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["sleep_sync_boundary_preserved"] is False


def test_unique_device_free_cannot_be_replaced_by_destroy_stream(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    aclrtStream stream = nullptr;\n"
        "    void* buffer = nullptr;\n"
        "    aclrtMalloc(&buffer, 64, ACL_MEM_MALLOC_HUGE_FIRST);\n"
        "    aclrtFree(buffer);\n"
        "    aclrtDestroyStream(stream);\n"
        "    const bool verify =",
    )
    candidate = source.replace("    aclrtFree(buffer);\n", "")
    candidate = candidate.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    path = tmp_path / "main.cpp"
    path.write_text(source, encoding="utf-8")
    snapshot = guard.make_snapshot(path)
    path.write_text(candidate, encoding="utf-8")
    result = guard.check_snapshot(snapshot, path)
    assert result["valid"] is False
    assert result["checks"]["cleanup_mutations_allowed"] is False


def test_workspace_golden_task_and_cmake_are_immutable(tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    (workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    for name in ("Golden.json", "TASK.md", "CMakeLists.txt"):
        (workspace / name).write_text("baseline\n", encoding="utf-8")
        snapshot = guard.make_snapshot(workspace)
        (workspace / name).write_text("changed\n", encoding="utf-8")
        result = guard.check_snapshot(snapshot, workspace)
        assert result["valid"] is False, name
        assert result["checks"]["protected_files_unchanged"] is False
        (workspace / name).write_text("baseline\n", encoding="utf-8")


def test_verified_and_passed_predicates_are_protected(tmp_path):
    for name in ("verified", "passed", "data_verified"):
        source = SOURCE.replace("verify", name)
        path = tmp_path / "main.cpp"
        path.write_text(source)
        snapshot = guard.make_snapshot(path)
        path.write_text(
            source.replace(
                "aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)"
            ).replace("query_rc == ACL_SUCCESS && device == 0", "true")
        )
        assert guard.check_snapshot(snapshot, path)["valid"] is False


def test_carrier_return_cannot_be_bypassed_or_deleted(tmp_path):
    source = SOURCE.replace("int main()", "int run_activation_carrier()")
    source = source.replace("return 0;", "return query_rc == ACL_SUCCESS ? 0 : 1;")
    path = tmp_path / "main.cpp"
    path.write_text(source)
    snapshot = guard.make_snapshot(path)
    repaired = source.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)")
    path.write_text(repaired)
    assert guard.check_snapshot(snapshot, path)["valid"] is True
    for replacement in ("return 0;", ""):
        path.write_text(
            repaired.replace("return query_rc == ACL_SUCCESS ? 0 : 1;", replacement)
        )
        result = guard.check_snapshot(snapshot, path)
        assert result["valid"] is False
        assert result["checks"]["return_semantics_unchanged"] is False


def test_workspace_main_repair_preserves_all_other_sources(tmp_path):
    main = tmp_path / "main.cpp"
    source = (
        "".join(
            f'#include "utils{suffix}"\n'
            for suffix in sorted(guard.WORKSPACE_SOURCE_SUFFIXES)
        )
        + SOURCE
    )
    main.write_text(source, encoding="utf-8")
    for suffix in sorted(guard.WORKSPACE_SOURCE_SUFFIXES):
        (tmp_path / f"utils{suffix}").write_text(
            "bool compare_results(int a, int b) { return a == b; }\n",
            encoding="utf-8",
        )
    snapshot = guard.make_snapshot(tmp_path)
    assert snapshot["source_files"] == {
        path.name: guard._sha256(path.read_bytes()) for path in tmp_path.iterdir()
    }
    main.write_text(
        source.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)"),
        encoding="utf-8",
    )

    result = guard.check_snapshot(snapshot, tmp_path)

    assert result["valid"] is True, result
    assert result["checks"]["source_files_within_scope"] is True


def test_workspace_included_sources_do_not_change_host_source_selection(tmp_path):
    (tmp_path / "case.cpp").write_text(SOURCE, encoding="utf-8")
    for suffix in guard.WORKSPACE_SOURCE_SUFFIXES - guard.SOURCE_SUFFIXES:
        (tmp_path / f"main{suffix}").write_text(
            "// included source\n", encoding="utf-8"
        )

    snapshot = guard.make_snapshot(tmp_path)

    assert snapshot["source_relative"] == "case.cpp"
    assert set(snapshot["source_files"]) == {path.name for path in tmp_path.iterdir()}


@pytest.mark.parametrize("suffix", [".cpp", ".h", ".hpp", ".asc"])
@pytest.mark.parametrize("mutation", ["modify", "add", "delete"])
def test_workspace_rejects_other_source_changes_during_main_repair(
    tmp_path, mutation, suffix
):
    main = tmp_path / "main.cpp"
    helper = tmp_path / f"utils{suffix}"
    source = f'#include "{helper.name}"\n' + SOURCE.replace(
        "device == 0", "compare_results(device, 0)"
    )
    main.write_text(source, encoding="utf-8")
    helper.write_text(
        "bool compare_results(int a, int b) { return a == b; }\n", encoding="utf-8"
    )
    snapshot = guard.make_snapshot(tmp_path)
    main.write_text(
        source.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)"),
        encoding="utf-8",
    )
    if mutation == "modify":
        helper.write_text(
            "bool compare_results(int a, int b) { return true; }\n", encoding="utf-8"
        )
    elif mutation == "add":
        (tmp_path / f"extra{suffix}").write_text("int extra() { return 0; }\n")
    else:
        helper.unlink()

    result = guard.check_snapshot(snapshot, tmp_path)

    assert result["valid"] is False
    assert result["checks"]["runtime_semantics_changed"] is True
    assert result["checks"]["source_files_within_scope"] is False
    assert result["checks"]["immutable_verification_surface_unchanged"] is False
    assert {error["rule"] for error in result["errors"]} == {
        "source_files_within_scope"
    }


@pytest.mark.parametrize("legacy", [True, False])
def test_workspace_rejects_snapshot_without_main_source_hash(tmp_path, legacy):
    (tmp_path / "main.cpp").write_text(SOURCE, encoding="utf-8")
    snapshot = guard.make_snapshot(tmp_path)
    if legacy:
        del snapshot["source_files"]
    else:
        snapshot["source_files"] = {}

    with pytest.raises(ValueError, match="source_files; remake the snapshot"):
        guard.check_snapshot(snapshot, tmp_path)


def test_workspace_rejects_oversized_secondary_source(tmp_path):
    (tmp_path / "main.cpp").write_text(SOURCE, encoding="utf-8")
    (tmp_path / "utils.cpp").write_bytes(b" " * (guard.MAX_SOURCE_BYTES + 1))

    with pytest.raises(ValueError, match="exceeds"):
        guard.make_snapshot(tmp_path)


@pytest.mark.parametrize("suffix", [".cpp", ".h", ".hpp", ".asc"])
def test_workspace_rejects_secondary_source_replaced_by_symlink(tmp_path, suffix):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    (workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    helper = workspace / f"utils{suffix}"
    helper.write_text("bool compare_results() { return false; }\n", encoding="utf-8")
    snapshot = guard.make_snapshot(workspace)
    target = tmp_path / "outside.cpp"
    target.write_text("bool compare_results() { return true; }\n", encoding="utf-8")
    helper.unlink()
    helper.symlink_to(target)

    with pytest.raises(ValueError, match="symlink"):
        guard.check_snapshot(snapshot, workspace)


def test_workspace_ignores_generated_and_vcs_directories(tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    (workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    ignored_names = (
        ".git",
        ".hg",
        ".svn",
        "__pycache__",
        ".pytest_cache",
        ".mypy_cache",
        ".ruff_cache",
        ".tox",
        ".venv",
        "venv",
        "node_modules",
        "build",
        "build-fast",
        "cmake-build-debug",
    )
    for name in ignored_names:
        directory = workspace / name
        directory.mkdir()
        (directory / "verify.bin").write_bytes(b"ignored protected bytes")
        (directory / "generated.cpp").write_text("ignored source", encoding="utf-8")

    result = guard.make_snapshot(workspace)
    files = guard._workspace_files(workspace)

    assert result["source_relative"] == "main.cpp"
    assert result["protected_files"] == {}
    assert {record.relative for record in files} == {"main.cpp"}


def test_workspace_rejects_ambiguous_sources_without_root_main(tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    (workspace / "first.cpp").write_text(SOURCE, encoding="utf-8")
    (workspace / "second.cxx").write_text(SOURCE, encoding="utf-8")

    with pytest.raises(ValueError, match="ambiguous"):
        guard.make_snapshot(workspace)


def test_workspace_prefers_unique_root_main_source(tmp_path):
    workspace = tmp_path / "workspace"
    nested = workspace / "nested"
    nested.mkdir(parents=True)
    (workspace / "main.c").write_text(SOURCE, encoding="utf-8")
    (nested / "other.cpp").write_text(SOURCE, encoding="utf-8")

    snapshot = guard.make_snapshot(workspace)

    assert snapshot["source_relative"] == "main.c"


@pytest.mark.parametrize("suffix", [".c", ".cc", ".cpp", ".cxx"])
def test_workspace_accepts_supported_source_suffixes(tmp_path, suffix):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    source = workspace / f"case{suffix}"
    source.write_text(SOURCE, encoding="utf-8")

    snapshot = guard.make_snapshot(workspace)

    assert snapshot["source_relative"] == source.name


def test_workspace_skips_unrelated_file_and_directory_symlinks(tmp_path, monkeypatch):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    (workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    external_file = tmp_path / "external-file.bin"
    external_file.write_bytes(b"not a protected asset")
    external_directory = tmp_path / "external-directory"
    external_directory.mkdir()
    (external_directory / "verify.bin").write_bytes(b"must not be visited")
    (external_directory / "source.cpp").write_text("must not be read", encoding="utf-8")
    file_link = workspace / "unrelated-file-link"
    directory_link = workspace / "unrelated-directory-link"
    file_link.symlink_to(external_file)
    directory_link.symlink_to(external_directory, target_is_directory=True)

    for method_name in ("resolve", "stat", "open", "read_bytes", "read_text"):
        original = getattr(Path, method_name)

        def guarded(path, *args, _original=original, _method=method_name, **kwargs):
            if _method == "stat" and kwargs.get("follow_symlinks") is False:
                return _original(path, *args, **kwargs)
            if any(
                path == link or link in path.parents
                for link in (file_link, directory_link)
            ):
                raise AssertionError(f"unrelated symlink target was accessed: {path}")
            return _original(path, *args, **kwargs)

        monkeypatch.setattr(Path, method_name, guarded)

    snapshot = guard.make_snapshot(workspace)

    assert snapshot["source_relative"] == "main.cpp"
    assert snapshot["protected_files"] == {}


def test_workspace_rejects_source_and_protected_symlinks(tmp_path):
    source_workspace = tmp_path / "source-workspace"
    source_workspace.mkdir()
    (source_workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    (source_workspace / "extra.cc").symlink_to(tmp_path / "missing-source-target")
    with pytest.raises(ValueError, match="symlink"):
        guard.make_snapshot(source_workspace)

    protected_workspace = tmp_path / "protected-workspace"
    protected_workspace.mkdir()
    (protected_workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    (protected_workspace / "verify.bin").symlink_to(
        tmp_path / "missing-protected-target"
    )
    with pytest.raises(ValueError, match="symlink"):
        guard.make_snapshot(protected_workspace)


def test_workspace_rejects_symlink_root(tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    (workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    workspace_link = tmp_path / "workspace-link"
    workspace_link.symlink_to(workspace, target_is_directory=True)

    with pytest.raises(ValueError, match="symlink"):
        guard.make_snapshot(workspace_link)


def test_check_snapshot_rejects_final_symlink_source_relative(tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    source = workspace / "main.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    snapshot = guard.make_snapshot(workspace)
    moved_source = workspace / "moved.cpp"
    source.rename(moved_source)
    source.symlink_to(moved_source)

    with pytest.raises(ValueError, match="symlink"):
        guard.check_snapshot(snapshot, workspace)


def test_check_snapshot_rejects_intermediate_symlink_source_relative(
    tmp_path, monkeypatch
):
    workspace = tmp_path / "workspace"
    real_directory = workspace / "real"
    real_directory.mkdir(parents=True)
    source = real_directory / "main.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    snapshot = guard.make_snapshot(workspace)
    (workspace / "alias").symlink_to(real_directory, target_is_directory=True)
    snapshot["source_relative"] = "alias/main.cpp"

    reads = []
    real_read = os.read

    def track_read(fd, count):
        reads.append(fd)
        return real_read(fd, count)

    monkeypatch.setattr(guard.os, "read", track_read)
    with pytest.raises(ValueError, match="symlink"):
        guard.check_snapshot(snapshot, workspace)
    assert reads == []


def test_workspace_rejects_out_of_root_source_relative(tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    (workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    snapshot = guard.make_snapshot(workspace)
    snapshot["source_relative"] = "../outside.cpp"

    with pytest.raises(ValueError, match="canonical POSIX relative path"):
        guard.check_snapshot(snapshot, workspace)


def test_workspace_rejects_oversized_source_and_protected_asset(tmp_path):
    source_workspace = tmp_path / "source-workspace"
    source_workspace.mkdir()
    (source_workspace / "main.cpp").write_bytes(b" " * (guard.MAX_SOURCE_BYTES + 1))
    with pytest.raises(ValueError, match="exceeds"):
        guard.make_snapshot(source_workspace)

    protected_workspace = tmp_path / "protected-workspace"
    protected_workspace.mkdir()
    (protected_workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    (protected_workspace / "verify").write_bytes(b"x" * (guard.MAX_PROTECTED_BYTES + 1))
    with pytest.raises(ValueError, match="exceeds"):
        guard.make_snapshot(protected_workspace)


def test_extensionless_protected_asset_is_hashed_and_mutation_rejected(tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    source = workspace / "main.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    protected = workspace / "verify" / "judge"
    protected.parent.mkdir()
    protected.write_bytes(b"\x00baseline")
    snapshot = guard.make_snapshot(workspace)
    source.write_text(
        SOURCE.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)"),
        encoding="utf-8",
    )
    protected.write_bytes(b"\x00changed")

    result = guard.check_snapshot(snapshot, workspace)

    assert "verify/judge" in snapshot["protected_files"]
    assert result["checks"]["protected_files_unchanged"] is False
    assert result["valid"] is False


def test_binary_protected_asset_is_hashed_and_mutation_rejected(tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    source = workspace / "main.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    protected = workspace / "Golden.bin"
    protected.write_bytes(b"\xff\x00\xfe")
    snapshot = guard.make_snapshot(workspace)
    source.write_text(
        SOURCE.replace("aclrtGetDevice(nullptr)", "aclrtGetDevice(&device)"),
        encoding="utf-8",
    )
    protected.write_bytes(b"\xff\x00\xfd")

    result = guard.check_snapshot(snapshot, workspace)

    assert "Golden.bin" in snapshot["protected_files"]
    assert result["checks"]["protected_files_unchanged"] is False
    assert result["valid"] is False
    assert not hasattr(guard, "PROTECTED_SUFFIXES")


def test_cli_missing_arguments_keeps_legacy_stderr_and_exit_one():
    result = subprocess.run(
        [sys.executable, str(Path(guard.__file__))],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 1
    assert result.stdout == ""
    assert "before and after source paths are required" in result.stderr


@pytest.mark.parametrize("output_format", ["json", "text"])
def test_cli_boundary_error_is_json_and_two_for_both_formats(tmp_path, output_format):
    before = tmp_path / "before.cpp"
    before.write_text(SOURCE, encoding="utf-8")
    after = tmp_path / "after.txt"
    after.write_text("private sentinel candidate body", encoding="utf-8")
    result = subprocess.run(
        [
            sys.executable,
            str(Path(guard.__file__)),
            "--format",
            output_format,
            str(before),
            str(after),
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 2
    assert result.stderr == ""
    assert len(result.stdout.splitlines()) == 1
    payload = json.loads(result.stdout)
    assert payload["schema_version"] == guard.SCHEMA_VERSION
    assert payload["valid"] is False
    assert "error" in payload
    assert "private sentinel candidate body" not in result.stdout


def test_atomic_open_rejects_explicit_guard_file_swapped_to_symlink(
    tmp_path, monkeypatch
):
    source = tmp_path / "source.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    sentinel = tmp_path / "outside.cpp"
    sentinel.write_bytes(b"unique-explicit-guard-sentinel")
    sentinel_info = sentinel.stat()
    replacement = tmp_path / "replacement-link"
    real_open = os.open
    real_read = os.read
    real_fstat = os.fstat
    open_flags = []
    read_identities = []

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

    monkeypatch.setattr(guard.os, "open", swap_then_open)
    monkeypatch.setattr(
        guard.os,
        "supports_dir_fd",
        _supports_dir_fd_with_open(swap_then_open, real_open),
    )
    monkeypatch.setattr(guard.os, "read", track_read)

    with pytest.raises((OSError, ValueError)):
        guard._read_bounded(
            source, root=None, max_bytes=guard.MAX_SOURCE_BYTES, expected=None
        )

    assert open_flags and open_flags[0] & os.O_NOFOLLOW
    assert (sentinel_info.st_dev, sentinel_info.st_ino) not in read_identities


def test_atomic_open_rejects_workspace_final_file_swap(tmp_path, monkeypatch):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    source = workspace / "main.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    sentinel = tmp_path / "outside.cpp"
    sentinel.write_bytes(b"unique-final-component-sentinel")
    sentinel_info = sentinel.stat()
    replacement = tmp_path / "replacement-link"
    real_open = os.open
    real_read = os.read
    real_fstat = os.fstat
    final_flags = []
    read_identities = []

    def swap_final_then_open(path, flags, *args, **kwargs):
        if kwargs.get("dir_fd") is not None and os.fspath(path) == "main.cpp":
            final_flags.append(flags)
            replacement.symlink_to(sentinel)
            os.replace(replacement, source)
        return real_open(path, flags, *args, **kwargs)

    def track_read(fd, count):
        info = real_fstat(fd)
        read_identities.append((info.st_dev, info.st_ino))
        return real_read(fd, count)

    monkeypatch.setattr(guard.os, "open", swap_final_then_open)
    monkeypatch.setattr(
        guard.os,
        "supports_dir_fd",
        _supports_dir_fd_with_open(swap_final_then_open, real_open),
    )
    monkeypatch.setattr(guard.os, "read", track_read)

    with pytest.raises((OSError, ValueError)):
        guard._read_bounded(
            source, root=workspace, max_bytes=guard.MAX_SOURCE_BYTES, expected=None
        )

    assert final_flags and final_flags[0] & os.O_NOFOLLOW
    assert (sentinel_info.st_dev, sentinel_info.st_ino) not in read_identities


def test_atomic_open_rejects_workspace_parent_swap(tmp_path, monkeypatch):
    workspace = tmp_path / "workspace"
    parent = workspace / "sub"
    parent.mkdir(parents=True)
    source = parent / "main.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    saved_parent = workspace / "sub-original"
    replacement_link = workspace / "replacement-link"
    outside = tmp_path / "outside"
    outside.mkdir()
    sentinel = outside / "main.cpp"
    sentinel.write_bytes(b"unique-parent-component-sentinel")
    real_open = os.open
    real_read = os.read
    opened_paths = []
    reads = []
    root_fd = None

    def swap_parent_then_open(path, flags, *args, **kwargs):
        nonlocal root_fd
        path_text = os.fspath(path)
        dir_fd = kwargs.get("dir_fd")
        if path_text == os.fspath(workspace) and dir_fd is None:
            fd = real_open(path, flags, *args, **kwargs)
            root_fd = fd
            opened_paths.append((path_text, flags, fd))
            return fd
        if path_text == "sub" and dir_fd == root_fd:
            opened_paths.append((path_text, flags, None))
            os.rename(parent, saved_parent)
            replacement_link.symlink_to(outside, target_is_directory=True)
            os.replace(replacement_link, parent)
        else:
            opened_paths.append((path_text, flags, None))
        return real_open(path, flags, *args, **kwargs)

    def track_read(fd, count):
        reads.append(fd)
        return real_read(fd, count)

    monkeypatch.setattr(guard.os, "open", swap_parent_then_open)
    monkeypatch.setattr(
        guard.os,
        "supports_dir_fd",
        _supports_dir_fd_with_open(swap_parent_then_open, real_open),
    )
    monkeypatch.setattr(guard.os, "read", track_read)

    with pytest.raises((OSError, ValueError)):
        guard._read_bounded(
            source, root=workspace, max_bytes=guard.MAX_SOURCE_BYTES, expected=None
        )

    sub_attempts = [item for item in opened_paths if item[0] == "sub"]
    assert len(sub_attempts) == 1
    assert sub_attempts[0][1] & os.O_NOFOLLOW
    assert sub_attempts[0][1] & os.O_DIRECTORY
    assert not any(item[0] == "main.cpp" for item in opened_paths)
    assert reads == []


def test_workspace_record_rejects_regular_inode_replacement(tmp_path, monkeypatch):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    (workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
    protected = workspace / "verify.bin"
    protected.write_bytes(b"original-protected-bytes")
    records = guard._workspace_files(workspace)
    record = next(item for item in records if item.path == protected)
    replacement = workspace / "replacement.tmp"
    replacement_bytes = b"unique-replacement-asset-body"
    replacement.write_bytes(replacement_bytes)
    replacement_info = replacement.stat()
    os.replace(replacement, protected)
    assert (record.stat.st_dev, record.stat.st_ino) != (
        replacement_info.st_dev,
        replacement_info.st_ino,
    )

    real_read_bounded = guard._read_bounded
    real_read = os.read
    expected_values = []
    read_chunks = []

    def record_expected(path, *, root, max_bytes, expected=None):
        expected_values.append(expected)
        return real_read_bounded(
            path, root=root, max_bytes=max_bytes, expected=expected
        )

    def track_read(fd, count):
        chunk = real_read(fd, count)
        read_chunks.append(chunk)
        return chunk

    monkeypatch.setattr(guard, "_read_bounded", record_expected)
    monkeypatch.setattr(guard.os, "read", track_read)

    with pytest.raises(ValueError, match="file changed while opening"):
        guard._protected_file_hashes(workspace, records)

    assert expected_values == [record.stat]
    assert expected_values[0] is record.stat
    assert replacement_bytes not in b"".join(read_chunks)


def test_streaming_read_rejects_guard_growth_past_limit(tmp_path, monkeypatch):
    source = tmp_path / "growing.cpp"
    source.write_bytes(b"a")
    limit = 31
    mutation_fd = os.open(source, os.O_WRONLY | os.O_APPEND)
    real_read = os.read
    returned = []
    growth = b"unique-guard-growth-payload-" + b"x" * 48
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
        monkeypatch.setattr(guard.os, "read", grow_after_first_read)
        with pytest.raises(ValueError, match="exceeds") as error:
            guard._read_bounded(source, root=None, max_bytes=limit, expected=None)
    finally:
        os.close(mutation_fd)

    assert sum(map(len, returned)) <= limit + 1
    assert growth.decode("ascii") not in str(error.value)


def test_streaming_read_rejects_guard_shrink_during_read(tmp_path, monkeypatch):
    source = tmp_path / "shrinking.cpp"
    source.write_bytes(b"secret-guard-source-" + b"a" * (128 * 1024))
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
        monkeypatch.setattr(guard.os, "read", shrink_after_first_read)
        with pytest.raises(ValueError, match="^file changed while reading$") as error:
            guard._read_bounded(
                source,
                root=None,
                max_bytes=guard.MAX_SOURCE_BYTES,
                expected=None,
            )
    finally:
        os.close(mutation_fd)

    assert len(returned[0]) == 64 * 1024
    assert "secret-guard-source" not in str(error.value)


def test_guard_closes_all_owned_fds_on_validation_or_read_error(tmp_path, monkeypatch):
    workspace = tmp_path / "workspace"
    parent = workspace / "sub"
    parent.mkdir(parents=True)
    source = parent / "main.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    real_open = os.open
    real_close = os.close
    real_fstat = os.fstat
    real_read = os.read

    for failure_stage in ("validation", "read"):
        opened = []
        closed = []
        final_fds = set()

        def record_open(path, flags, *args, **kwargs):
            fd = real_open(path, flags, *args, **kwargs)
            opened.append((os.fspath(path), fd))
            if os.fspath(path) == "main.cpp":
                final_fds.add(fd)
            return fd

        def record_close(fd):
            closed.append(fd)
            return real_close(fd)

        with monkeypatch.context() as scoped:
            scoped.setattr(guard.os, "open", record_open)
            scoped.setattr(
                guard.os,
                "supports_dir_fd",
                _supports_dir_fd_with_open(record_open, real_open),
            )
            scoped.setattr(guard.os, "close", record_close)
            if failure_stage == "validation":

                def fail_final_fstat(fd):
                    if fd in final_fds:
                        raise RuntimeError("injected final identity failure")
                    return real_fstat(fd)

                scoped.setattr(guard.os, "fstat", fail_final_fstat)
                expected_message = "injected final identity failure"
            else:

                def fail_final_read(fd, _count):
                    if fd in final_fds:
                        raise RuntimeError("injected file read failure")
                    return real_read(fd, _count)

                scoped.setattr(guard.os, "read", fail_final_read)
                expected_message = "injected file read failure"

            with pytest.raises(RuntimeError, match=expected_message):
                guard._read_bounded(
                    source,
                    root=workspace,
                    max_bytes=guard.MAX_SOURCE_BYTES,
                    expected=None,
                )

        owned_fds = [fd for _path, fd in opened]
        assert len(owned_fds) == 3
        assert len(set(owned_fds)) == 3
        assert closed == owned_fds or all(closed.count(fd) == 1 for fd in owned_fds)
        for fd in owned_fds:
            with pytest.raises(OSError) as error:
                os.fstat(fd)
            assert error.value.errno == errno.EBADF


@pytest.mark.parametrize("missing_capability", ["dir_fd", "nofollow"])
def test_secure_directory_open_fails_closed_without_dir_fd_or_nofollow(
    tmp_path, monkeypatch, missing_capability
):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    (workspace / "main.cpp").write_text(SOURCE, encoding="utf-8")
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

    with monkeypatch.context() as scoped:
        scoped.setattr(guard.os, "open", record_open)
        scoped.setattr(guard.os, "read", record_read)
        if missing_capability == "dir_fd":
            scoped.setattr(guard.os, "supports_dir_fd", set())
        else:
            scoped.delattr(guard.os, "O_NOFOLLOW")

        with pytest.raises(OSError) as error:
            guard._open_root_dir(workspace)

    assert error.value.errno == errno.ENOTSUP
    assert open_calls == []
    assert read_calls == []


# 未读取 Event 状态的查询不能代替被删掉的同步。
def test_ignored_event_query_cannot_justify_removing_stream_wait(tmp_path):
    source = SOURCE.replace(
        "    const bool verify =",
        "    aclrtStream stream = nullptr;\n"
        "    aclrtEvent event = nullptr;\n"
        "    aclrtEventRecordedStatus state;\n"
        "    aclrtRecordEvent(event, stream);\n"
        "    aclrtQueryEventStatus(event, &state);\n"
        "    std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
        "    aclrtSynchronizeStream(stream);\n"
        "    const bool verify =",
    )
    candidate = source.replace("    aclrtSynchronizeStream(stream);\n", "")
    result = check_pair(tmp_path, source, candidate)
    assert result["valid"] is False
    assert result["checks"]["sleep_sync_boundary_preserved"] is False
