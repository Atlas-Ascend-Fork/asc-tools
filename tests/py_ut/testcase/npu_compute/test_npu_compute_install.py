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
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


BUILD_DIR = Path(
    os.environ.get(
        "NPU_COMPUTE_TEST_BUILD_DIR",
        "/tmp/asc_tools_npu_compute_integration",
    )
)
BIN_DIR = Path(
    os.environ.get(
        "NPU_COMPUTE_TEST_BIN_DIR",
        str(BUILD_DIR / "bin"),
    )
)
INSTALL_ARCH = os.environ.get("NPU_COMPUTE_TEST_ARCH", platform.machine())


@pytest.fixture(scope="module")
def install_root(tmp_path_factory):
    prefix = tmp_path_factory.mktemp("asc_tools_default_install")
    result = subprocess.run(
        [
            "cmake",
            "--install",
            str(BUILD_DIR),
            "--prefix",
            str(prefix),
            "--component",
            "asc-tools",
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return prefix


@pytest.fixture(scope="module")
def public_install_root(install_root, tmp_path_factory):
    public_root = tmp_path_factory.mktemp("asc_tools_public_install")
    architecture_root = install_root / f"{INSTALL_ARCH}-linux"
    for directory in ("bin", "include", "lib64"):
        (public_root / directory).symlink_to(
            architecture_root / directory,
            target_is_directory=True,
        )
    return public_root


def test_default_component_installs_the_declared_layout(install_root):
    architecture_root = install_root / f"{INSTALL_ARCH}-linux"
    public_cli = architecture_root / "bin/npu-compute"
    private_cli = install_root / "tools/npu_tools/bin/npu-compute"
    private_library_root = install_root / "tools/npu_tools/lib64"
    assert public_cli.is_file()
    assert public_cli.read_bytes().startswith(b"#!/bin/sh\n")
    assert private_cli.is_file()
    assert private_cli.read_bytes().startswith(b"\x7fELF")
    for library in (
        "libnpu_compute_processor.so",
        "libacl_pti.so",
        "libacl_tool_injection.so",
    ):
        assert (private_library_root / library).is_file()
        assert not (architecture_root / "lib64" / library).exists()

    assert not (architecture_root / "include/aclpti").exists()
    assert not (architecture_root / "include/npu_compute/acl_pti.h").exists()
    assert not (architecture_root / "include/npu_compute/pti_data_module.h").exists()

    assert not (install_root / "share/npu-compute").exists()
    assert not (architecture_root / "bin/npu_compute_stub_demo_app").exists()
    assert not (architecture_root / "lib64/libpti_data_module.so").exists()
    assert not (architecture_root / "lib64/libpti_data_module_impl.so").exists()
    assert not (architecture_root / "lib64/libprofapi.so").exists()
    assert not (architecture_root / "lib64/libruntime.so").exists()

    aclpti_symbols = subprocess.run(
        [
            "nm",
            "-D",
            "--defined-only",
            "--demangle",
            str(private_library_root / "libacl_pti.so"),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert aclpti_symbols.returncode == 0, aclpti_symbols.stderr
    assert "aclptiRegisterProfilingDataCallback" in aclpti_symbols.stdout
    assert "aclptiRegisterDataModuleShutdownCallback" in aclpti_symbols.stdout
    for internal_namespace in (
        "activity",
        "callback",
        "data",
        "profiling",
        "handler",
    ):
        assert f"aclpti::{internal_namespace}::" not in aclpti_symbols.stdout


def test_public_cli_symlink_uses_the_architecture_injection_library(
    install_root, public_install_root
):
    cli = public_install_root / "bin/npu-compute"
    assert cli.is_file()
    program = "import os; print('ACL_API_INJECTION=' + os.environ['ACL_API_INJECTION'])"
    result = subprocess.run(
        [
            str(cli),
            "--section",
            "PipeUtilization",
            str(BIN_DIR / "npu_compute_rep_fixture_app"),
            "--mode",
            "exec",
            "--",
            sys.executable,
            "-c",
            program,
        ],
        cwd=public_install_root,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    expected = (
        install_root / "tools/npu_tools/lib64/libnpu_compute_processor.so"
    ).resolve()
    assert f"ACL_API_INJECTION={expected}" in result.stdout.splitlines()


def test_installed_layout_runs_the_minimal_stub_chain(install_root, tmp_path):
    # Keep the product installation intact for the layout checks. This test
    # runs the installed CLI with the same backends as the other stub tests.
    stub_install_root = tmp_path / "stub-install"
    shutil.copytree(install_root, stub_install_root, symlinks=True)
    architecture_root = stub_install_root / f"{platform.machine()}-linux"
    private_library_root = stub_install_root / "tools/npu_tools/lib64"
    library = private_library_root / "libnpu_compute_processor.so"
    library.unlink()
    shutil.copy2(BIN_DIR / "libnpu-compute-test.so", library)
    cli = architecture_root / "bin/npu-compute"
    assert cli.is_file()
    environment = os.environ.copy()
    search_paths = [str(private_library_root), str(BIN_DIR)]
    if environment.get("LD_LIBRARY_PATH"):
        search_paths.append(environment["LD_LIBRARY_PATH"])
    environment["LD_LIBRARY_PATH"] = ":".join(search_paths)
    environment.pop("NPU_COMPUTE_STUB_SUBSCRIBE_RESULT", None)
    environment["NPU_COMPUTE_DEBUG"] = "1"
    hardware_stub = Path(os.environ["NPU_COMPUTE_HARDWARE_API_STUB"])
    assert hardware_stub.is_file()
    environment["LD_PRELOAD"] = ":".join(
        value
        for value in (str(hardware_stub), environment.get("LD_PRELOAD", ""))
        if value
    )

    result = subprocess.run(
        [
            str(cli),
            "--section",
            "PipeUtilization",
            str(BIN_DIR / "npu_compute_stub_demo_app"),
        ],
        cwd=stub_install_root,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )

    # The stub exercises initialization and teardown without producing PMU rows.
    assert result.returncode == 3, result.stderr
    assert "valid results were packaged" in result.stderr
    assert "[libnpu-compute] subscriber initialized" in result.stderr
    assert "[demo] completed" in result.stderr
