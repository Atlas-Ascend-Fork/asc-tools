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
import sys
import time
import mmap
import json
import struct
import shutil
import tempfile
import argparse
from enum import Enum
from typing import List
from dataclasses import dataclass, field

from msobjdump import utils


KEY_ASCEND_META = ".ascend.meta."
KEY_ASCEND_META_OP = ".ascend.meta"
KEY_ASCEND_KERNEL = ".ascend.kernel."
KEY_AICORE_BINARY = ".aicore_binary"
KEY_O_JSON = "_binary_"
KEY_A_FILE = ".a"
KEY_O_FILE = ".cpp.o"
HEX_NUM = 16
############### _o_json define ####################
TYPE_START = "start"
TYPE_END = "end"
TYPE_SIZE = "size"
ACLNN_BINARY = ""

############### ascend kernel define ####################
KERNEL_TYPE_MAP = {"0": "mix", "1": "aiv", "2": "aic"}

############### Function Meta Type ####################
F_TYPE_KTYPE = 1
F_TYPE_CROSS_CORE_SYNC = 2
F_TYPE_MIX_TASK_RATION = 3
F_TYPE_ENABLE_EARLY_START = 11
F_TYPE_DETERMINISTIC_INFO = 13
F_TYPE_FUNCTION_ENTRY = 14
F_TYPE_BLOCK_NUM = 15

F_TYPE_MAP = {
    F_TYPE_KTYPE: "KERNEL_TYPE",
    F_TYPE_CROSS_CORE_SYNC: "CROSS_CORE_SYNC",
    F_TYPE_MIX_TASK_RATION: "MIX_TASK_RATION",
    F_TYPE_ENABLE_EARLY_START: "ENABLE_EARLY_START",
    F_TYPE_DETERMINISTIC_INFO: "DETERMINISTIC_INFO",
    F_TYPE_FUNCTION_ENTRY: "FUNCTION_ENTRY",
    F_TYPE_BLOCK_NUM: "BLOCK_NUM",
}

############### Binary Meta Type ####################
B_TYPE_VERSION = 0
B_TYPE_DEBUG = 1
B_TYPE_DYNAMIC_PARAM = 2
B_TYPE_OPTIONAL_PARAM = 3
B_TYPE_RUNTIME_IMPLICIT_INFO = 4

B_TYPE_MAP = {
    B_TYPE_VERSION: "VERSION",
    B_TYPE_DEBUG: "DEBUG",
    B_TYPE_DYNAMIC_PARAM: "DYNAMIC_PARAM",
    B_TYPE_OPTIONAL_PARAM: "OPTIONAL_PARAM",
    B_TYPE_RUNTIME_IMPLICIT_INFO: "RUNTIME_IMPLICIT_INFO",
}

K_TYPE_MAP = {
    "1": "AICORE",
    "2": "AIC",
    "3": "AIV",
    "4": "MIX_AIC_MAIN",
    "5": "MIX_AIV_MAIN",
    "6": "AIC_ROLLBACK",
    "7": "AIV_ROLLBACK",
}

DEVICE_MACHINE_PATTERN = re.compile(r"(?:0x1029|\bhiipu\b)", re.IGNORECASE)
SASS_INSTRUCTION_PATTERN = re.compile(r"^\s*[0-9a-fA-F]+:\s+[0-9a-fA-F]{8}\s+\S")
SASS_PLACEHOLDER_PATTERN = re.compile(r"<(?:not available|unknown)>", re.IGNORECASE)
ARCHIVE_MAGIC = b"!<arch>\n"
THIN_ARCHIVE_MAGIC = b"!<thin>\n"
MAX_SASS_CONTAINER_DEPTH = 4
C_TYPE_MAP = {"0": "NO_USE_SYNC", "1": "USE_SYNC"}
RUNTIME_IMPLICIT_INFO_MAP = {
    1: "SIMD Printf Flag",
    2: "Hardware Sync Flag",
    3: "L2Cache Hint Flag",
    4: "SIMT Printf Flag",
    5: "SIMD Assert Flag",
}


class ObjType(Enum):
    TYPE_ASCEND_KERNEL = 0
    TYPE_BINARY_O_JSON = 1
    TYPE_ASCEND_META = 2
    TYPE_AICORE_BINARY = 3


class ParseObjMode(Enum):
    MODE_DUMP_ELF = 0
    MODE_EXTRA_ELF = 1
    MODE_LIST_ELF = 2
    MODE_VERBOSE = 3
    MODE_SASS = 4


@dataclass
class AscendKernel:
    kernel_type: str
    kernel_len: int
    kernel_file: str


@dataclass
class AscendKernelInfos:
    version: str
    type_cnt: int
    kernels: List[AscendKernel] = field(default_factory=list)


@dataclass(frozen=True)
class SassImage:
    path: str
    source: str


@dataclass(frozen=True)
class SassDiagnostic:
    source: str
    message: str


class ObjDump:
    """
    objdump tool manager
    ====================
    支持简易工程打包交付件、aclnn打包交付件、单算子编译交付件的解压解析
    ====================
    """

    def __init__(self, args):
        self.obj = None  # 待解析解压文件
        self.src_obj = None  # 用户原始输入文件
        self.parse_obj_mode = None  # ParseObjMode 解析解压类型
        self.obj_type = None  # ObjType obj文件场景分类
        self.tmp_dir = None  # 存放临时文件 结束后目录会删除
        self.out_dir = getattr(
            args, "out_dir", None
        )  # 落盘文件目录，用户未设置时当前路径
        self._aicore_binary_meta_printed = set()
        self._sass_tools = {}
        try:
            self._set_out_dir()
            # preprocess args
            self._set_parse_obj_and_mode(args)
        except Exception:
            self._clean()
            raise

    @staticmethod
    def _show_ascend_meta_tlv(content: bytes, t: int, tlv_len: int, index: int):
        if t == F_TYPE_MIX_TASK_RATION:
            v1, v2 = struct.unpack("2H", content[index : index + 4])
            print(f"{F_TYPE_MAP.get(t)}: [{v1}:{v2}]")
        elif t == F_TYPE_CROSS_CORE_SYNC:
            (v,) = struct.unpack("I", content[index : index + 4])
            print(f"{F_TYPE_MAP.get(t)}: {C_TYPE_MAP.get(str(v))}")
        elif t == F_TYPE_KTYPE:
            (v,) = struct.unpack("I", content[index : index + 4])
            print(f"{F_TYPE_MAP.get(t)}: {K_TYPE_MAP.get(str(v))}")
        elif t == F_TYPE_DETERMINISTIC_INFO:
            (v,) = struct.unpack("I", content[index : index + 4])
            print(f"{F_TYPE_MAP.get(t)}: {v}")
        elif t == F_TYPE_ENABLE_EARLY_START:
            (v,) = struct.unpack("I", content[index : index + 4])
            print(f"{F_TYPE_MAP.get(t)}: {v}")
        elif t == F_TYPE_FUNCTION_ENTRY:
            (v,) = struct.unpack("<Q", content[index + 4 : index + 12])
            print(f"{F_TYPE_MAP.get(t)}: {v}")
        elif t == F_TYPE_BLOCK_NUM:
            v = "0xFFFFFFFF"
            print(f"{F_TYPE_MAP.get(t)}: {v}")

    @staticmethod
    def _unpack_buff_content_by_type(
        content: bytes, start_idx: int, read_len: int, type_str: str
    ) -> int:
        error_message = "[ERROR]: Failed to parse Ascend kernel content: buffer access is out of bounds."
        end_idx = start_idx + read_len
        if start_idx < 0 or read_len < 0 or end_idx > len(content):
            raise RuntimeError(error_message)
        try:
            return struct.unpack(type_str, content[start_idx:end_idx])[0]
        except struct.error as error:
            raise RuntimeError(error_message) from error

    def run(self):
        try:
            return self._parse_process()
        finally:
            self._clean()

    def _show_ascend_meta_op_tlv(
        self, content: bytes, t: int, tlv_len: int, index: int
    ):
        if t == B_TYPE_VERSION:
            (v,) = struct.unpack("I", content[index : index + 4])
            output = f"{B_TYPE_MAP.get(t)}: {v}"
            self._print_ascend_meta_op_tlv(output)
        if t == B_TYPE_DEBUG:
            debugBufSize, debugOptions = struct.unpack("II", content[index : index + 8])
            output = f"{B_TYPE_MAP.get(t)}: debugBufSize={debugBufSize}, debugOptions={debugOptions}"
            self._print_ascend_meta_op_tlv(output)
        elif t == B_TYPE_DYNAMIC_PARAM:
            (dynamicParamMode,) = struct.unpack("H", content[index + 2 : index + 4])
            output = f"{B_TYPE_MAP.get(t)}: dynamicParamMode={dynamicParamMode}"
            self._print_ascend_meta_op_tlv(output)
        elif t == B_TYPE_OPTIONAL_PARAM:
            optionalInputMode, optionalOutputMode = struct.unpack(
                "HH", content[index : index + 4]
            )
            output = (
                f"{B_TYPE_MAP.get(t)}: optionalInputMode={optionalInputMode}, "
                f"optionalOutputMode={optionalOutputMode}"
            )
            self._print_ascend_meta_op_tlv(output)
        elif t == B_TYPE_RUNTIME_IMPLICIT_INFO:
            (v,) = struct.unpack("I", content[index : index + 4])
            output = f"{B_TYPE_MAP.get(t)}: {RUNTIME_IMPLICIT_INFO_MAP.get(v, v)}"
            self._print_ascend_meta_op_tlv(output)

    def _print_ascend_meta_op_tlv(self, output: str):
        if self.obj_type == ObjType.TYPE_AICORE_BINARY:
            if output in self._aicore_binary_meta_printed:
                return
            self._aicore_binary_meta_printed.add(output)
        print(output)

    def _set_out_dir(self):
        if not self.out_dir:
            self.out_dir = os.getcwd()
        self.out_dir = os.path.realpath(self.out_dir)
        if not os.path.isdir(self.out_dir):
            raise RuntimeError(f"[ERROR]: output dir {(self.out_dir)} is invalid path!")
        try:
            self.tmp_dir = tempfile.mkdtemp(
                prefix="objdump_"
                + time.strftime("%Y%m%d_%H%M%S")
                + "_"
                + str(os.getpid())
                + "_",
                dir=self.out_dir,
            )
        except PermissionError as e:
            raise PermissionError(
                f"[ERROR]: Cannot create temporary directory under {self.out_dir} "
                "for saving tmp file, please check user permission or disk space."
            ) from e
        except OSError as e:
            raise RuntimeError(
                f"[ERROR]: Cannot create temporary directory under {self.out_dir} "
                "for saving tmp file, please check user permission or disk space."
            ) from e

    def _set_parse_obj_and_mode(self, args):
        sass_obj = getattr(args, "sass", None)
        if isinstance(sass_obj, str) and sass_obj:
            self.obj = sass_obj
            self.src_obj = sass_obj
            self.parse_obj_mode = ParseObjMode.MODE_SASS
            return

        dump_elf = getattr(args, "dump_elf", None)
        extr_elf = getattr(args, "extr_elf", None)
        list_elf = getattr(args, "list_elf", None)
        verbose = getattr(args, "verbose", False)
        if not dump_elf and not extr_elf and not list_elf:
            print("[ERROR]: command check error, please check !!!")
            return

        if dump_elf:
            if verbose:
                self.parse_obj_mode = ParseObjMode.MODE_VERBOSE
                self.obj = dump_elf
            else:
                self.obj = dump_elf
                self.parse_obj_mode = ParseObjMode.MODE_DUMP_ELF
        elif extr_elf:
            self.obj = extr_elf
            self.parse_obj_mode = ParseObjMode.MODE_EXTRA_ELF
        elif list_elf:
            self.obj = list_elf
            self.parse_obj_mode = ParseObjMode.MODE_LIST_ELF

        self.src_obj = self.obj
        self._detect_obj_type()
        if self.obj_type is not None and self.obj_type != ObjType.TYPE_AICORE_BINARY:
            return

        if self.obj_type == ObjType.TYPE_AICORE_BINARY:
            self.obj = self._extract_aicore_binary()

    def _detect_obj_type(self):
        self.obj_type = None
        output = utils.get_symbols_in_file(self.obj)

        if "_o_start" in output or "_json_start" in output:
            self.obj_type = ObjType.TYPE_BINARY_O_JSON
        output = utils.get_section_headers_in_file(self.obj)

        if KEY_ASCEND_META in output:
            self.obj_type = ObjType.TYPE_ASCEND_META
        if KEY_ASCEND_KERNEL in output:
            self.obj_type = ObjType.TYPE_ASCEND_KERNEL
        if KEY_AICORE_BINARY in output:
            self.obj_type = ObjType.TYPE_AICORE_BINARY

    def _extract_aicore_binary(
        self, input_file: str = None, objcopy_path: str = None
    ) -> str:
        source_file = input_file or self.obj
        source_name_file = input_file or getattr(self, "src_obj", None)
        src_name = (
            os.path.basename(source_name_file)
            if source_name_file
            else "fusion_aicore_binary"
        )
        if input_file is None:
            tmp_file = os.path.join(self.tmp_dir, f"{src_name}.aicore.o")
        else:
            tmp_fd, tmp_file = tempfile.mkstemp(
                prefix="aicore_", suffix=".o", dir=self.tmp_dir
            )
            os.close(tmp_fd)
        if os.path.exists(tmp_file):
            os.remove(tmp_file)
        try:
            if objcopy_path:
                result = utils.extract_aicore_binary_from_elf_with_tool(
                    source_file, tmp_file, objcopy_path
                )
            else:
                result = utils.extract_aicore_binary_from_elf(source_file, tmp_file)
        except FileNotFoundError as e:
            raise RuntimeError(
                "[ERROR]: llvm-objcopy is not available, cannot extract .aicore_binary."
            ) from e

        if result.returncode != 0:
            err_msg = result.stderr.strip() if result.stderr else "unknown error"
            raise RuntimeError(f"[ERROR]: Extract .aicore_binary failed: {err_msg}")

        if not os.path.exists(tmp_file) or os.path.getsize(tmp_file) == 0:
            raise RuntimeError(
                "[ERROR]: Extracted .aicore_binary file is empty or missing."
            )
        return tmp_file

    @staticmethod
    def _is_archive(input_file: str) -> bool:
        try:
            with open(input_file, "rb") as file_obj:
                magic = file_obj.read(len(ARCHIVE_MAGIC))
            return magic in (ARCHIVE_MAGIC, THIN_ARCHIVE_MAGIC)
        except OSError:
            return False

    @staticmethod
    def _is_thin_archive(input_file: str) -> bool:
        try:
            with open(input_file, "rb") as file_obj:
                return file_obj.read(len(THIN_ARCHIVE_MAGIC)) == THIN_ARCHIVE_MAGIC
        except OSError:
            return False

    @staticmethod
    def _completed_process_text(result) -> str:
        content = getattr(result, "stdout", "")
        if isinstance(content, bytes):
            return content.decode("utf-8", errors="replace")
        return content or ""

    @staticmethod
    def _completed_process_error(result) -> str:
        content = getattr(result, "stderr", "")
        if isinstance(content, bytes):
            return content.decode("utf-8", errors="replace").strip()
        return (content or "").strip()

    @staticmethod
    def _sass_error_message(error: Exception) -> str:
        """去掉旧接口已经添加的前缀，避免 Sass 诊断出现重复的 [ERROR]。"""
        message = str(error).strip()
        while message.startswith("[ERROR]:"):
            message = message[len("[ERROR]:") :].strip()
        return message or error.__class__.__name__

    def _get_sass_tool(self, tool_name: str) -> str:
        if tool_name not in self._sass_tools:
            tool_path = shutil.which(tool_name)
            if not tool_path:
                raise RuntimeError(
                    f"[ERROR]: {tool_name} is not available, please source the matching CANN environment."
                )
            self._sass_tools[tool_name] = os.path.realpath(tool_path)
        return self._sass_tools[tool_name]

    @staticmethod
    def _check_device_elf(header: str, sections: str) -> bool:
        """检查设备 ELF（处理流程第 4 步）。

        以文件头 Machine 及 .text 段内容判断，不依赖扩展名，
        也不要求必须有 .ascend.meta 或符号表。
        """
        return bool(DEVICE_MACHINE_PATTERN.search(header)) and bool(
            re.search(r"\s\.text\s+", sections)
        )

    def _classify_sass_file(self, input_file: str) -> str:
        """输入分类（处理流程第 2 步），返回统一的 FileKind 分叉值。"""
        if self._is_archive(input_file):
            return "thin_archive" if self._is_thin_archive(input_file) else "archive"

        header_result = utils.get_elf_header(input_file)
        if getattr(header_result, "returncode", 1) != 0:
            return "unknown"

        header = self._completed_process_text(header_result)
        sections = utils.get_section_headers_in_file(input_file)
        if DEVICE_MACHINE_PATTERN.search(header):
            return "device" if self._check_device_elf(header, sections) else "unknown"

        if KEY_AICORE_BINARY in sections:
            return "aicore_binary"
        if KEY_ASCEND_KERNEL in sections:
            return "ascend_kernel"

        symbols = utils.get_symbols_in_file(input_file)
        if "_o_start" in symbols or "_json_start" in symbols:
            return "binary_o_json"
        return "host"

    def _new_sass_temp_file(self, prefix: str, suffix: str = ".o") -> str:
        tmp_fd, tmp_file = tempfile.mkstemp(
            prefix=prefix, suffix=suffix, dir=self.tmp_dir
        )
        os.close(tmp_fd)
        return tmp_file

    def _collect_sass_images(
        self, input_file: str, source: str = None, depth: int = 0
    ) -> tuple:
        """枚举与提取设备镜像（处理流程第 3 步）：输入分类后的统一分叉点。

        每种容器一个提取器，提取结果回到本函数重新分类，直至设备 ELF 或
        host/unknown 终结；来源链随 depth 传递用于镜像标注。
        """
        source = source or input_file
        images = []
        diagnostics = []
        if depth > MAX_SASS_CONTAINER_DEPTH:
            return images, [
                SassDiagnostic(source, "nested device container depth exceeded")
            ]

        try:
            kind = self._classify_sass_file(input_file)
        except (OSError, RuntimeError, ValueError) as error:
            return images, [SassDiagnostic(source, self._sass_error_message(error))]

        if kind == "thin_archive":
            return images, [SassDiagnostic(source, "thin archive is not supported")]
        if kind == "archive":
            return self._expand_archive(input_file, source, depth)
        if kind == "device":
            return [SassImage(os.path.realpath(input_file), source)], diagnostics
        if kind == "aicore_binary":
            return self._expand_aicore_binary(input_file, source, depth)
        if kind == "ascend_kernel":
            return self._expand_ascend_kernel(input_file, source, depth)
        if kind == "binary_o_json":
            return self._expand_binary_o_json(input_file, source, depth)
        return images, diagnostics

    def _expand_archive(self, input_file: str, source: str, depth: int) -> tuple:
        """归档提取器：遍历全部成员（ar t / ar p），逐成员递归。"""
        images = []
        diagnostics = []
        result = utils.list_archive_members(input_file)
        if getattr(result, "returncode", 1) != 0:
            error = self._completed_process_error(result) or "ar t failed"
            return images, [SassDiagnostic(source, error)]

        members = [
            line for line in self._completed_process_text(result).splitlines() if line
        ]
        if len(members) != len(set(members)):
            return images, [
                SassDiagnostic(source, "archive contains duplicate member names")
            ]

        for index, member in enumerate(members):
            member_file = self._new_sass_temp_file(f"archive_{index}_", ".member")
            member_result = utils.extract_archive_member(
                input_file, member, member_file
            )
            if getattr(member_result, "returncode", 1) != 0:
                error = self._completed_process_error(member_result) or "ar p failed"
                diagnostics.append(SassDiagnostic(f"{source}({member})", error))
                continue
            child_images, child_diagnostics = self._collect_sass_images(
                member_file, f"{source}({member})", depth + 1
            )
            images.extend(child_images)
            diagnostics.extend(child_diagnostics)
        return images, diagnostics

    def _expand_aicore_binary(self, input_file: str, source: str, depth: int) -> tuple:
        """融合封装提取器：llvm-objcopy 提取 .aicore_binary 后递归。"""
        try:
            objcopy_path = self._get_sass_tool("llvm-objcopy")
            extracted = self._extract_aicore_binary(input_file, objcopy_path)
        except (OSError, RuntimeError, ValueError, KeyError, IndexError) as error:
            return [], [SassDiagnostic(source, self._sass_error_message(error))]
        return self._collect_sass_images(
            extracted, f"{source}[{KEY_AICORE_BINARY}]", depth + 1
        )

    def _expand_ascend_kernel(self, input_file: str, source: str, depth: int) -> tuple:
        """.ascend.kernel.* 封装提取器：提取各 kernel 文件后逐个递归。"""
        images = []
        diagnostics = []
        try:
            kernel_infos = self._parse_ascend_kernel_infos(input_file)
            output_dir = tempfile.mkdtemp(prefix="ascend_kernel_", dir=self.tmp_dir)
            extracted_files = self._parse_elf_ascend_kernel_by_type(
                kernel_infos, "dump", input_file, output_dir
            )
            for kernel_info_group in extracted_files:
                for kernel_info in kernel_info_group.kernels:
                    child_images, child_diagnostics = self._collect_sass_images(
                        kernel_info.kernel_file,
                        f"{source}[{KEY_ASCEND_KERNEL}{kernel_info.kernel_type}]",
                        depth + 1,
                    )
                    images.extend(child_images)
                    diagnostics.extend(child_diagnostics)
            if not images and not extracted_files:
                diagnostics.append(
                    SassDiagnostic(source, "no device ELF in .ascend.kernel sections")
                )
        except (OSError, RuntimeError, ValueError, KeyError, IndexError) as error:
            diagnostics.append(SassDiagnostic(source, self._sass_error_message(error)))
        return images, diagnostics

    def _expand_binary_o_json(self, input_file: str, source: str, depth: int) -> tuple:
        """对象/JSON 符号打包提取器：恢复各 .o 负载后递归，JSON 不作输入。"""
        images = []
        diagnostics = []
        try:
            point_content = self._parse_binary_o_json_obj(input_file)
            output_dir = tempfile.mkdtemp(prefix="binary_o_json_", dir=self.tmp_dir)
            save_files = self._save_dump_elf_o_json(
                point_content, input_file, warn=False, output_dir=output_dir
            )
            for file_name, file_path in save_files.items():
                if not file_name.endswith(".o"):
                    continue
                child_images, child_diagnostics = self._collect_sass_images(
                    file_path, f"{source}[{file_name}]", depth + 1
                )
                images.extend(child_images)
                diagnostics.extend(child_diagnostics)
            if not images:
                diagnostics.append(
                    SassDiagnostic(source, "no device ELF in _o/_json payload")
                )
        except (OSError, RuntimeError, ValueError, KeyError, IndexError) as error:
            diagnostics.append(SassDiagnostic(source, self._sass_error_message(error)))
        return images, diagnostics

    @staticmethod
    def _sass_instruction_lines(output: str) -> list:
        return [
            line for line in output.splitlines() if SASS_INSTRUCTION_PATTERN.match(line)
        ]

    def _disassemble_sass_image(self, image: SassImage, objdump_path: str) -> tuple:
        try:
            result = utils.disassemble_with_llvm_objdump(image.path, objdump_path)
        except FileNotFoundError as error:
            return "", f"llvm-objdump is not available: {error}"

        output = self._completed_process_text(result)
        error = self._completed_process_error(result)
        if getattr(result, "returncode", 1) != 0:
            return output, error or "llvm-objdump -d failed"

        instruction_lines = self._sass_instruction_lines(output)
        if not instruction_lines:
            return output, "llvm-objdump produced no valid instruction lines"

        placeholder_lines = [
            line for line in instruction_lines if SASS_PLACEHOLDER_PATTERN.search(line)
        ]
        if placeholder_lines:
            if len(placeholder_lines) == len(instruction_lines):
                return output, "llvm-objdump produced no usable device instructions"
            return output, "llvm-objdump output contains unknown instructions"
        return output, ""

    def _sass_process(self) -> int:
        """--sass 处理流程（各步骤独立成方法，便于定位与测试）：

        参数校验（parse_args / SassFileAction）→ 输入分类与统一分叉
        （_classify_sass_file / _collect_sass_images）→ 检查设备 ELF
        （_check_device_elf）→ 调用 llvm-objdump -d（_disassemble_sass_image）→
        汇总输出及状态（_report_sass_results）→ 清理临时文件（run 的 finally）。
        """
        objdump_path = self._get_sass_tool("llvm-objdump")
        images, diagnostics = self._collect_sass_images(
            self.src_obj, os.path.realpath(self.src_obj)
        )
        if not images:
            diagnostics.append(
                SassDiagnostic(
                    os.path.realpath(self.src_obj), "no device ELF found in input"
                )
            )
        return self._report_sass_results(images, diagnostics, objdump_path)

    def _report_sass_results(self, images, diagnostics, objdump_path: str) -> int:
        """汇总输出及状态：镜像逐个反汇编输出，诊断进 stderr，集中计算退出码。

        退出码 0 要求：找到镜像、无任何诊断、无反汇编错误；
        多镜像部分失败时保留成功部分输出，整体仍返回 1。
        """
        for diagnostic in diagnostics:
            print(
                f"[ERROR]: {diagnostic.source}: {diagnostic.message}",
                file=sys.stderr,
            )

        has_disassembly_error = False
        for image in images:
            output, error = self._disassemble_sass_image(image, objdump_path)
            if output and self._sass_instruction_lines(output):
                print(f"===== [SASS] {image.source} =====")
                print(output, end="" if output.endswith("\n") else "\n")
            if error:
                has_disassembly_error = True
                print(f"[ERROR]: {image.source}: {error}", file=sys.stderr)

        return 0 if images and not diagnostics and not has_disassembly_error else 1

    def _parse_process(self):
        if self.parse_obj_mode == ParseObjMode.MODE_SASS:
            return self._sass_process()
        if (
            self.parse_obj_mode == ParseObjMode.MODE_DUMP_ELF
            or self.parse_obj_mode == ParseObjMode.MODE_VERBOSE
        ):
            self._dump_elf_process()
        elif self.parse_obj_mode == ParseObjMode.MODE_EXTRA_ELF:
            self._extra_elf()
        elif self.parse_obj_mode == ParseObjMode.MODE_LIST_ELF:
            self._list_elf()
        return 0

    def _clean(self):
        if self.tmp_dir and os.path.exists(self.tmp_dir):
            try:
                shutil.rmtree(self.tmp_dir)
            except OSError as error:
                print(
                    f"[WARNING]: failed to clean temporary directory {self.tmp_dir}: {error}",
                    file=sys.stderr,
                )

    def _dump_elf_process(self):
        if self.obj_type == ObjType.TYPE_BINARY_O_JSON:
            point_content_list = self._parse_binary_o_json_obj()
            save_files = self._save_dump_elf_o_json(point_content_list)
            for file_name in save_files:
                if file_name.endswith(".o"):
                    self.obj = save_files[file_name]
                    self._show_elf_ascend_meta_obj()
            if self.parse_obj_mode == ParseObjMode.MODE_VERBOSE:
                self._show_binary_o_json_obj(save_files)
        elif self.obj_type == ObjType.TYPE_ASCEND_KERNEL:
            ascend_kernel_dict = self._parse_ascend_kernel_infos()
            kernel_obj_infos = self._parse_elf_ascend_kernel_by_type(
                ascend_kernel_dict, "dump"
            )
            self._show_elf_ascend_kernel_obj(kernel_obj_infos)
        elif self.obj_type == ObjType.TYPE_ASCEND_META:
            self._show_elf_ascend_meta_obj()
        elif self.obj_type == ObjType.TYPE_AICORE_BINARY:
            self._show_elf_ascend_meta_obj()
            if self.parse_obj_mode == ParseObjMode.MODE_VERBOSE:
                print("====== [elf header infos] ======")
                print(utils.get_all_section_symbols_in_file(self.obj))
        else:
            print("The kernel meta information cannot be found.")

    def _extra_elf(self):
        if self.obj_type == ObjType.TYPE_BINARY_O_JSON:
            point_content_list = self._parse_binary_o_json_obj()
            save_files = self._save_dump_elf_o_json(point_content_list)
            self._move_file_to_outdir_o_json(save_files)
        elif self.obj_type == ObjType.TYPE_ASCEND_KERNEL:
            ascend_kernel_dict = self._parse_ascend_kernel_infos()
            kernel_obj_infos = self._parse_elf_ascend_kernel_by_type(
                ascend_kernel_dict, "dump"
            )
            self._move_file_to_outdir_ascend_kernel(kernel_obj_infos)
        elif self.obj_type == ObjType.TYPE_ASCEND_META:
            print("[WARNING]: nothing to extra in single op elf file")
        elif self.obj_type == ObjType.TYPE_AICORE_BINARY:
            self._move_extracted_aicore_binary_to_outdir()

    def _list_elf(self):
        if self.obj_type == ObjType.TYPE_BINARY_O_JSON:
            point_content_list = self._parse_binary_o_json_obj()
            self._list_elf_info_aclnn_pkg_obj(point_content_list)
        elif self.obj_type == ObjType.TYPE_ASCEND_KERNEL:
            ascend_kernel_dict = self._parse_ascend_kernel_infos()
            self._parse_elf_ascend_kernel_by_type(ascend_kernel_dict, "list")
        elif self.obj_type == ObjType.TYPE_ASCEND_META:
            print("[WARNING]: nothing to list in single op elf file")
        elif self.obj_type == ObjType.TYPE_AICORE_BINARY:
            self._list_extracted_aicore_binary()

    def _list_extracted_aicore_binary(self):
        print(f"ELF file    0: {os.path.basename(self.obj)}")

    def _move_extracted_aicore_binary_to_outdir(self):
        utils.copy_file_src_exist(
            self.obj, os.path.join(self.out_dir, os.path.basename(self.obj))
        )

    def _copy_file_o_json_to_out_dir(
        self, src_file: str, json_file: str, copy_files: list
    ):
        utils.copy_file_src_exist(src_file, os.path.join(self.out_dir, json_file))
        copy_files.append(src_file)
        tmp_o_file = src_file.replace(".json", ".o")
        o_file = json_file.replace(".json", ".o")
        if os.path.exists(tmp_o_file):
            utils.copy_file_src_exist(tmp_o_file, os.path.join(self.out_dir, o_file))
            copy_files.append(tmp_o_file)

    def _copy_file_with_file_name_manual(
        self, base_name: str, tmp_file: str, copy_files: list
    ):
        new_file_name = self._get_file_name_with_path(base_name)
        if new_file_name != base_name:
            self._copy_file_o_json_to_out_dir(tmp_file, new_file_name, copy_files)

    def _move_file_to_outdir_o_json(self, tmp_files: dict):
        copy_files = []
        for base_name, tmp_file in tmp_files.items():
            if not base_name.endswith(".json"):
                continue
            with open(tmp_file, "r") as f:
                op_json = json.load(f)

            if op_json.get("filePath", None):
                file_json = op_json["filePath"]
                self._copy_file_o_json_to_out_dir(tmp_file, file_json, copy_files)
            else:
                self._copy_file_with_file_name_manual(base_name, tmp_file, copy_files)

        for base_name, tmp_file in tmp_files.items():
            if tmp_file not in copy_files:
                utils.copy_file_src_exist(
                    tmp_file, os.path.join(self.out_dir, base_name)
                )

    def _move_file_to_outdir_ascend_kernel(
        self, kernel_obj_info_list: List[AscendKernelInfos]
    ):
        for kernel_obj_infos in kernel_obj_info_list:
            for kernel_info in kernel_obj_infos.kernels:
                tmp_file = kernel_info.kernel_file
                utils.copy_file_src_exist(
                    tmp_file, os.path.join(self.out_dir, os.path.basename(tmp_file))
                )

    def _get_file_name_with_path(self, base_name: str):
        name_list = base_name.split("_")
        dir_names = [
            item
            for item in name_list
            if utils.is_prefix_substring(item, ["config", "ascend"])
        ]
        if not dir_names:
            return base_name
        dir_path = os.path.join(*dir_names)
        file_idx = name_list.index(dir_names[-1]) + 1
        if file_idx >= len(name_list):
            return base_name
        file_name = "_".join(name_list[file_idx:])
        return os.path.join(dir_path, file_name)

    def _parse_binary_o_json_obj(self, input_file: str = None) -> dict:
        """
        解析各个_o _json的 地址偏移信息, 并把各个obj内容落盘到临时文件中
        """
        point_content_list = {}
        output = utils.get_symbols_in_file(input_file or self.obj)

        for line in output.split("\n"):
            if KEY_O_JSON not in line:
                continue
            if line.endswith("_" + TYPE_START):
                line_list = utils.split_str_with_space(line)
                line_name = utils.get_str_between(line, KEY_O_JSON, "_" + TYPE_START)
                if line_name not in point_content_list:
                    point_content_list[line_name] = {}
                point_content_list[line_name][TYPE_START] = hex(
                    int(line_list[1], HEX_NUM)
                )
            elif line.endswith("_" + TYPE_END):
                line_list = utils.split_str_with_space(line)
                line_name = utils.get_str_between(line, KEY_O_JSON, "_" + TYPE_END)
                if line_name not in point_content_list:
                    point_content_list[line_name] = {}
                point_content_list[line_name][TYPE_END] = hex(
                    int(line_list[1], HEX_NUM)
                )
            elif line.endswith("_" + TYPE_SIZE):
                line_list = utils.split_str_with_space(line)
                line_name = utils.get_str_between(line, KEY_O_JSON, "_" + TYPE_SIZE)
                if line_name not in point_content_list:
                    point_content_list[line_name] = {}
                point_content_list[line_name][TYPE_SIZE] = hex(
                    int(line_list[1], HEX_NUM)
                )
        return point_content_list

    def _show_binary_o_json_obj(self, device_files: dict):
        for file_name, file_path in device_files.items():
            if file_name.endswith(".json"):
                continue
            output = utils.get_all_section_symbols_in_file(file_path)
            print(f"===== [elf header infos] in {file_name} =====:")
            for line in output.split("\n"):
                print(line)

    def _list_elf_info_aclnn_pkg_obj(self, point_content_dict: dict):
        file_idx = 0
        for obj_name, _ in point_content_dict.items():
            file_name = self._get_file_name_o_json(obj_name)
            print(f"ELF file    {str(file_idx)}: {file_name}")
            file_idx += 1

    def _parse_ascend_kernel_infos(self, input_file: str = None) -> dict:
        """
        解析的.ascend.kernel. 地址偏移信息
        """
        ascend_kernel_infos = {}
        output = utils.get_section_headers_in_file(input_file or self.obj)
        for line in output.split("\n"):
            if KEY_ASCEND_KERNEL not in line:
                continue
            line = line.split(KEY_ASCEND_KERNEL)[1]
            line_list = utils.split_str_with_space(line)

            ascend_kernel_name_id = 0
            addr_offsize = 2
            offset_offsize = 3
            size_offsize = 4
            ascend_kernel_name = line_list[ascend_kernel_name_id].replace(".", "_")
            ascend_kernel_addr = hex(
                int(line_list[ascend_kernel_name_id + addr_offsize], HEX_NUM)
            )
            ascend_kernel_offset = hex(
                int(line_list[ascend_kernel_name_id + offset_offsize], HEX_NUM)
            )
            ascend_kernel_size = hex(
                int(line_list[ascend_kernel_name_id + size_offsize], HEX_NUM)
            )
            ascend_kernel_infos[ascend_kernel_name] = [
                ascend_kernel_addr,
                ascend_kernel_offset,
                ascend_kernel_size,
            ]
        return ascend_kernel_infos

    def _show_elf_ascend_kernel_obj(
        self, kernel_obj_info_list: List[AscendKernelInfos]
    ):
        """
        打屏显示elf文件信息、section段、symbols
        """
        for kernel_obj_infos in kernel_obj_info_list:
            print("===========================")
            print(f"[VERSION]: {kernel_obj_infos.version}")
            print(f"[TYPE COUNT]: {kernel_obj_infos.type_cnt}")
            print("===========================")
            for idx, kernel_info in enumerate(kernel_obj_infos.kernels):
                file_name = os.path.basename(kernel_info.kernel_file)
                print(f"[ELF FILE {idx}]: {file_name}")
                print(f"[KERNEL TYPE]: {kernel_info.kernel_type}")
                print(f"[KERNEL LEN]: {kernel_info.kernel_len}")
                print(f"[ASCEND META]: {self._show_elf_ascend_meta_obj()}")
                if self.parse_obj_mode == ParseObjMode.MODE_VERBOSE:
                    print("====== [elf header infos] ======")
                    print(
                        utils.get_all_section_symbols_in_file(kernel_info.kernel_file)
                    )

    def _get_elf_ascend_meta_tlv(self, meta_infos: dict):
        """
        接收.ascend.meta. 字典数据， offset偏移获取TLV起始地址, size 获取TLV数据长度
        循环遍历获取TLV数据并打印映射信息
        """
        meta_item_num = 0
        for meta_name, meta_lists in meta_infos.items():
            if meta_name == "meta":
                continue
            content = self._get_segment_content(
                int(meta_lists[1], HEX_NUM), int(meta_lists[2], HEX_NUM)
            )
            index = 0
            print(f"{KEY_ASCEND_META} [{meta_item_num}]: {meta_name}")
            while index < len(content):
                if index + 4 > len(content):
                    break

                t, tlv_len = struct.unpack("2H", content[index : index + 4])
                index += 4
                if (index + tlv_len) <= len(content):
                    self._show_ascend_meta_tlv(content, t, tlv_len, index)
                index += tlv_len

    def _get_elf_ascend_meta_op_tlv(self, meta_infos: dict):
        meta_name = "meta"
        if meta_name in meta_infos.keys():
            self._aicore_binary_meta_printed.clear()
            meta_lists = meta_infos[meta_name]
            content = self._get_segment_content(
                int(meta_lists[1], HEX_NUM), int(meta_lists[2], HEX_NUM)
            )
            idx = 0
            index = 0
            print(f"{KEY_ASCEND_META_OP} META INFO")
            while index < len(content) and (
                idx < 4 or self.obj_type == ObjType.TYPE_AICORE_BINARY
            ):
                if index + 4 > len(content):
                    break
                t, tlv_len = struct.unpack("2H", content[index : index + 4])
                index += 4
                if index + tlv_len > len(content):
                    break
                self._show_ascend_meta_op_tlv(content, t, tlv_len, index)
                index += tlv_len
                idx += 1

    def _show_elf_ascend_meta_obj(self):
        """
        展示.ascend.meta段信息
        """
        meta_infos = {}
        output = utils.get_section_headers_in_file(self.obj)
        for line in output.split("\n"):
            if KEY_ASCEND_META in line or KEY_ASCEND_META_OP in line:
                if KEY_ASCEND_META in line:
                    line = line.split(KEY_ASCEND_META)[1]
                else:
                    line = line.split(".ascend.")[1]
                line_list = utils.split_str_with_space(line)
                meta_name_id = 0
                meta_name = line_list[meta_name_id]
                meta_addr = hex(int(line_list[meta_name_id + 2], HEX_NUM))
                meta_offset = hex(int(line_list[meta_name_id + 3], HEX_NUM))
                meta_size = hex(int(line_list[meta_name_id + 4], HEX_NUM))
                meta_infos[meta_name] = [meta_addr, meta_offset, meta_size]
        self._get_elf_ascend_meta_op_tlv(meta_infos)
        self._get_elf_ascend_meta_tlv(meta_infos)

    def _get_file_name_o_json(self, obj_name: str) -> list:
        file_name = (
            obj_name[: obj_name.rfind("_")] + "." + obj_name[obj_name.rfind("_") + 1 :]
        )
        return file_name

    def _save_dump_elf_o_json(
        self,
        point_content_dict: dict,
        input_file: str = None,
        warn: bool = True,
        output_dir: str = None,
    ) -> dict:
        """
        保存data段各个elf的内容到临时文件下
            文件名示例: ascend910b_add_custom_AddCustom_c43818e8e69f92d25146c434c100f58a.json:
            若是xxxx_o: 保存为.o
            若是xxxx_json: 保存为.json
        """
        save_files = {}
        source_file = input_file or self.obj
        data_addr, data_offset, data_size = get_data_segment_range(source_file)
        if data_size == 0:
            if warn:
                print(
                    f"[WARNING]: there is no .data section in {source_file}, please check input elf file."
                )
            return {}
        for obj_name, point_content in point_content_dict.items():
            obj_start_offset = (
                int(point_content[TYPE_START], HEX_NUM) + data_offset - data_addr
            )
            obj_size = int(point_content[TYPE_SIZE], HEX_NUM)
            file_name = self._get_file_name_o_json(obj_name)
            tmp_file = os.path.join(output_dir or self.tmp_dir, file_name)

            content = self._get_segment_content(obj_start_offset, obj_size, source_file)
            if file_name.endswith(".json"):
                content = json.loads(content.decode("utf-8"))
                with open(tmp_file, "a") as f:
                    json.dump(content, f, indent=4)
            else:
                with open(tmp_file, "ab") as f:
                    f.write(content)
            save_files[file_name] = tmp_file
        return save_files

    def _parse_elf_ascend_kernel_by_type(
        self,
        point_content_dict: dict,
        parse_type: str,
        input_file: str = None,
        output_dir: str = None,
    ) -> list:
        """
        保存.ascend.kernel段各个elf的内容到临时文件下
            文件名示例: ascend910b_add_custom_AddCustom_c43818e8e69f92d25146c434c100f58a.json:
            若是xxxx_o: 保存为.o
            若是xxxx_json: 保存为.json
        """
        kernel_infos = []
        for obj_name, point_content in point_content_dict.items():
            start_offset = int(point_content[1], HEX_NUM)
            content_size = int(point_content[2], HEX_NUM)
            content = self._get_segment_content(start_offset, content_size, input_file)
            kernel_info = self._parse_ascend_kernel_content(
                content, obj_name, parse_type, output_dir
            )
            kernel_infos.append(kernel_info)
        return kernel_infos

    def _parse_ascend_kernel_content(
        self,
        content: bytes,
        obj_name: str,
        parse_type: str,
        output_dir: str = None,
    ) -> AscendKernelInfos:
        """
        解析.ascend.kernel section的内容
        内容落盘, 返回生成的AscendKernelInfos信息

        每个kernel条目包含缓冲区分配长度（kernel_len）和实际ELF文件长度（kernel_len_real）。
        前者决定下一个条目的偏移，后者决定写出的字节数。
        """
        total_len = len(content)
        read_len = 0
        kernel_infos = AscendKernelInfos(0, 0)
        read_head_len = 8
        if read_len + read_head_len < total_len:
            version = struct.unpack("I", content[read_len : read_len + 4])[0]
            read_len += 4
            type_cnt = struct.unpack("I", content[read_len : read_len + 4])[0]
            read_len += 4

            kernel_infos = AscendKernelInfos(version, type_cnt)
            for kernel_id in range(type_cnt):
                # parse info
                kernel_type = self._unpack_buff_content_by_type(
                    content, read_len, 4, "I"
                )
                kernel_type = KERNEL_TYPE_MAP.get(str(kernel_type), "unknow")
                read_len += 4
                kernel_len = self._unpack_buff_content_by_type(
                    content, read_len, 4, "I"
                )
                read_len += 4
                kernel_len_real = self._unpack_buff_content_by_type(
                    content, read_len, 4, "I"
                )
                read_len += 4
                if kernel_len_real > kernel_len or read_len + kernel_len > total_len:
                    raise RuntimeError(
                        "[ERROR]: Failed to parse Ascend kernel content: "
                        "buffer access is out of bounds. "
                        f"obj_name={obj_name}, kernel_id={kernel_id}"
                    )
                # parse buff
                file_name = "_".join([obj_name, str(kernel_id), kernel_type]) + ".o"
                if parse_type == "list":
                    print("ELF file    " + str(kernel_id) + ": " + file_name)
                else:
                    file_name = os.path.join(output_dir or self.tmp_dir, file_name)
                    with open(file_name, "ab") as f:
                        f.write(content[read_len : read_len + kernel_len_real])
                read_len += kernel_len
                # record info in kernel_infos
                ascend_kernel = AscendKernel(kernel_type, kernel_len_real, file_name)
                kernel_infos.kernels.append(ascend_kernel)

        return kernel_infos

    def _get_segment_content(self, offset: int, size: int, input_file: str = None):
        """
        获取data段上指定偏移量和大小的内容
        """
        source_file = input_file or self.obj
        file_size = int(os.path.getsize(source_file))
        with open(source_file, "rb") as f:
            with mmap.mmap(f.fileno(), file_size, access=mmap.ACCESS_READ) as mm:
                return mm[offset : (offset + size)]


def get_data_segment_range(filename: str):
    """
    获取data段的起始地址和大小
    """
    output = utils.get_section_headers_in_file(filename)
    for line in output.split("\n"):
        if " .data " in line:
            parts = line.strip().split()
            try:
                data_index = parts.index(".data")
                return (
                    int(parts[data_index + 2], HEX_NUM),
                    int(parts[data_index + 3], HEX_NUM),
                    int(parts[data_index + 4], HEX_NUM),
                )
            except (ValueError, IndexError):
                continue
    return 0, 0, 0


def run_obj_dump(args):
    """
    Total entry for objdump tool
    支持多用户同时调用
    """
    objdump = ObjDump(args)
    return objdump.run()


def extract_values_from_parentheses(string):
    pattern = r"\((.*?)\)"
    match = re.search(pattern, string)
    if match:
        return match.group(1)
    else:
        return None


def get_o_file_name(file_path):
    output = utils.get_all_section_symbols_in_file(file_path)
    for line in output.split("\n"):
        if KEY_O_FILE in line:
            file_name = extract_values_from_parentheses(line)
        if KEY_ASCEND_KERNEL in line:
            utils.get_o_file_from_a_file(file_path, file_name)
            return file_name
    return None


def get_o_file(values):
    try:
        file_name = get_o_file_name(values)
        return os.path.realpath(file_name)
    except FileNotFoundError:
        print("[ERROR]: The specified file or directory does not exist!!!")
    except PermissionError:
        print("[ERROR]: Permission denied!!!")
    except Exception:
        print("[ERROR]: The input file or directory does not exist!!!")
    return values


class FileAction(argparse.Action):
    """ """

    def __call__(self, parser, namespace, values, option_string=None):
        """
        文件有效性检查
        """
        try:
            if values.endswith(KEY_A_FILE):
                get_o_file(values)
                values = get_o_file(values)
            if not values or not os.path.exists(values):
                raise RuntimeError(
                    f"[ERROR]: File({values}) does not exist or permission denied!!!"
                )
            else:
                setattr(namespace, self.dest, os.path.realpath(values))
        except RuntimeError:
            print("[ERROR]: File does not exist or permission denied!!!")


class SassFileAction(argparse.Action):
    """只校验 Sass 输入路径，不对 archive 做旧逻辑的首成员提取。"""

    def __call__(self, parser, namespace, values, option_string=None):
        if not values or not os.path.exists(values):
            raise argparse.ArgumentError(
                self,
                f"File({values}) does not exist or permission denied",
            )
        setattr(namespace, self.dest, os.path.realpath(values))


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        prog="msobjdump", description="objdump tool for Ascend C elf file"
    )
    elf_operator_group = parser.add_argument_group()
    elf_operator_group.add_argument(
        "--dump-elf",
        "-d",
        dest="dump_elf",
        required=False,
        metavar="",
        action=FileAction,
        help="Dump ELF Object brief informations.",
    )
    elf_operator_group.add_argument(
        "--verbose",
        "-V",
        dest="verbose",
        required=False,
        action="store_true",
        default=False,
        help="Dump ELF Object all section informations.",
    )
    elf_operator_group.add_argument(
        "--extract-elf",
        "-e",
        dest="extr_elf",
        required=False,
        metavar="",
        action=FileAction,
        help="Extract ELF file(s) name containing <partial file name> and save as file(s).",
    )
    elf_operator_group.add_argument(
        "--list-elf",
        "-l",
        dest="list_elf",
        required=False,
        metavar="",
        action=FileAction,
        help="List all the ELF files available in the fatbin.",
    )
    elf_operator_group.add_argument(
        "--sass",
        dest="sass",
        required=False,
        metavar="<input>",
        action=SassFileAction,
        help="Disassemble Ascend AICore instructions with llvm-objdump.",
    )
    parser.add_argument(
        "--out-dir",
        "-o",
        dest="out_dir",
        required=False,
        help="Set user output dir path.",
    )

    parser.set_defaults(entry_function=run_obj_dump)
    if argv is None:
        argv = sys.argv[1:]
    if not argv:
        parser.print_help()
        parser.exit(0)

    args = parser.parse_args(argv)

    if args.sass:
        if args.dump_elf or args.extr_elf or args.list_elf:
            parser.error("--sass cannot be combined with an existing ELF action")
        if args.verbose or args.out_dir is not None:
            parser.error("--sass cannot be combined with --verbose or --out-dir")
    elif not (args.dump_elf or args.extr_elf or args.list_elf):
        parser.error(
            "one of --dump-elf, --extract-elf, --list-elf, or --sass is required"
        )

    return args


def _run_cli(args):
    try:
        return args.entry_function(args)
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1


def main():
    args = parse_args()
    return _run_cli(args)


if __name__ == "__main__":
    sys.exit(main())
