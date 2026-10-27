#!/usr/bin/python
# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

import os
import re
import shutil
import subprocess


def _command_env() -> dict:
    """为外部 ELF 工具提供稳定的文本输出环境。"""
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    env["LANG"] = "C"
    return env


def split_str_with_space(input_str: str) -> list:
    result = re.split(r"\s+", input_str)
    return [element for element in result if element != ""]


def get_str_between(input_str: str, sub_str_s: str, sub_str_e: str) -> str:
    pattern = rf"{sub_str_s}(.*?){sub_str_e}"
    result = re.search(pattern, input_str)
    if result:
        return result.group(1)
    return ""


def is_prefix_substring(str_check: str, str_lst: list) -> bool:
    for lst_str in str_lst:
        if str_check.lower().startswith(lst_str.lower()):
            return True
    return False


def copy_file_src_exist(src_file: str, dest_file: str) -> None:
    if not os.path.exists(src_file):
        return
    if not os.path.exists(os.path.dirname(dest_file)):
        os.makedirs(os.path.dirname(dest_file))
    shutil.copy(src_file, dest_file)


def get_section_headers_in_file(file_name: str) -> str:
    return subprocess.run(
        ["readelf", "-SW", file_name],
        capture_output=True,
        text=True,
        shell=False,
        env=_command_env(),
    ).stdout


def get_symbols_in_file(file_name: str) -> str:
    return subprocess.run(
        ["readelf", "-sW", file_name],
        capture_output=True,
        text=True,
        shell=False,
        env=_command_env(),
    ).stdout


def get_all_section_symbols_in_file(file_name: str) -> str:
    return subprocess.run(
        ["readelf", "-aW", file_name],
        capture_output=True,
        text=True,
        shell=False,
        env=_command_env(),
    ).stdout


def get_o_file_from_a_file(file_path: str, file_name: str) -> str:
    return subprocess.run(
        ["ar", "x", file_path, file_name],
        capture_output=True,
        shell=False,
        env=_command_env(),
    )


def extract_aicore_binary_from_elf(
    input_file: str, output_file: str, objcopy_path: str = None
):
    return extract_aicore_binary_from_elf_with_tool(
        input_file, output_file, objcopy_path
    )


def extract_aicore_binary_from_elf_with_tool(
    input_file: str, output_file: str, objcopy_path: str = None
):
    """从 ELF 中提取 .aicore_binary，保留旧接口的两个参数调用方式。"""
    return subprocess.run(
        [
            objcopy_path or "llvm-objcopy",
            "-O",
            "binary",
            "--only-section=.aicore_binary",
            input_file,
            output_file,
        ],
        capture_output=True,
        text=True,
        shell=False,
        env=_command_env(),
    )


def get_elf_header(file_name: str):
    """读取 ELF header，调用方负责解释返回内容。"""
    return subprocess.run(
        ["readelf", "-hW", file_name],
        capture_output=True,
        text=True,
        shell=False,
        env=_command_env(),
    )


def list_archive_members(file_name: str):
    """按 archive 原始顺序列出成员。"""
    return subprocess.run(
        ["ar", "t", file_name],
        capture_output=True,
        text=True,
        shell=False,
        env=_command_env(),
    )


def extract_archive_member(archive_name: str, member_name: str, output_file: str):
    """将单个 archive 成员写入指定临时文件，不改变当前工作目录。"""
    with open(output_file, "wb") as output:
        return subprocess.run(
            ["ar", "p", archive_name, member_name],
            stdout=output,
            stderr=subprocess.PIPE,
            shell=False,
            env=_command_env(),
        )


def disassemble_with_llvm_objdump(
    file_name: str, objdump_path: str, lineinfo: bool = False, source: bool = False
):
    """调用 CANN llvm-objdump 反汇编一个设备 ELF。"""
    command = [objdump_path, "-d"]
    if lineinfo:
        command.append("-l")
    if source:
        command.append("-S")
    command.append(file_name)
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        shell=False,
        env=_command_env(),
    )
