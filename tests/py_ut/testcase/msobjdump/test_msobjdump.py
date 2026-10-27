#! /usr/bin/env python3
# -*- coding: UTF-8 -*-
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
import sys
import shutil
import struct
import subprocess
import tempfile
import unittest
import contextlib
import importlib
from types import SimpleNamespace

from unittest.mock import MagicMock, patch
from io import StringIO

THIS_FILE_NAME = __file__
FILE_PATH = os.path.dirname(os.path.realpath(THIS_FILE_NAME))
MSOBJDUMP_PATH = os.path.join(FILE_PATH, "../../../../", "utils/msobjdump")
print(MSOBJDUMP_PATH)
sys.path = [MSOBJDUMP_PATH] + sys.path
msobjdump_main = importlib.import_module("msobjdump.msobjdump_main")
utils = importlib.import_module("msobjdump.utils")


class TestMsObjdump(unittest.TestCase):
    def setUp(self):
        # 每个测试用例执行之前做操作
        print("---------------------set up case----------------------------------")

    def tearDown(self):
        # 每个测试用例执行之后做操作
        print("---------------------tear down case-------------------------------")

    def test_unpack_buff_content_by_type(self):
        content = struct.pack("I", 7)

        result = msobjdump_main.ObjDump._unpack_buff_content_by_type(
            content, 0, len(content), "I"
        )

        self.assertEqual(result, 7)

    def test_unpack_buff_content_by_type_out_of_bounds(self):
        with self.assertRaisesRegex(RuntimeError, "buffer access is out of bounds"):
            msobjdump_main.ObjDump._unpack_buff_content_by_type(b"\x00", 0, 4, "I")

    @patch("msobjdump.msobjdump_main.mmap.mmap")
    @patch("msobjdump.msobjdump_main.open")
    @patch("msobjdump.msobjdump_main.os.path.getsize", return_value=4)
    def test_get_segment_content_opens_read_only(
        self, mock_getsize, mock_open, mock_mmap
    ):
        obj_dump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        obj_dump.obj = "input.o"
        mapped_content = MagicMock()
        mock_mmap.return_value = mapped_content

        obj_dump._get_segment_content(1, 2)

        mock_open.assert_called_once_with("input.o", "rb")
        mock_mmap.assert_called_once_with(
            mock_open.return_value.__enter__.return_value.fileno(),
            4,
            access=msobjdump_main.mmap.ACCESS_READ,
        )

    def test_parse_ascend_kernel_content_rejects_real_length_larger_than_allocated(
        self,
    ):
        obj_dump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        obj_dump.tmp_dir = tempfile.mkdtemp()
        # version=1, type_cnt=1, type=aiv, allocated length=2, actual length=3
        content = struct.pack("5I", 1, 1, 1, 2, 3) + b"ab"

        try:
            with self.assertRaisesRegex(RuntimeError, "obj_name=kernel, kernel_id=0"):
                obj_dump._parse_ascend_kernel_content(content, "kernel", "dump")
        finally:
            shutil.rmtree(obj_dump.tmp_dir)

    def test_parse_ascend_kernel_content_rejects_truncated_payload(self):
        obj_dump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        obj_dump.tmp_dir = tempfile.mkdtemp()
        # version=1, type_cnt=1, type=aiv, allocated/actual length=3, payload has 2 bytes
        content = struct.pack("5I", 1, 1, 1, 3, 3) + b"ab"

        try:
            with self.assertRaisesRegex(RuntimeError, "obj_name=kernel, kernel_id=0"):
                obj_dump._parse_ascend_kernel_content(content, "kernel", "dump")
        finally:
            shutil.rmtree(obj_dump.tmp_dir)

    def test_file_action_only_parses_a_suffix_as_archive(self):
        action = msobjdump_main.FileAction([], "file")
        with tempfile.TemporaryDirectory() as temp_dir:
            axx_file = os.path.join(temp_dir, "xxx.axx")
            a_file = os.path.join(temp_dir, "xxx.a")
            open(axx_file, "w").close()
            open(a_file, "w").close()

            with patch.object(
                msobjdump_main, "get_o_file", side_effect=lambda value: value
            ) as mock_get_o_file:
                action(None, SimpleNamespace(), axx_file)
                self.assertEqual(mock_get_o_file.call_count, 0)

                action(None, SimpleNamespace(), a_file)
                self.assertEqual(mock_get_o_file.call_count, 2)

    def test_parse_args_supports_sass_without_short_alias(self):
        args = msobjdump_main.parse_args(["--sass", __file__])

        self.assertEqual(args.sass, os.path.realpath(__file__))
        self.assertFalse(args.verbose)
        with self.assertRaises(SystemExit) as context:
            msobjdump_main.parse_args(["-sass", __file__])
        self.assertEqual(context.exception.code, 2)

    def test_parse_args_rejects_sass_with_existing_action(self):
        with self.assertRaises(SystemExit) as context:
            msobjdump_main.parse_args(["--sass", __file__, "--verbose"])

        self.assertEqual(context.exception.code, 2)

    def test_sass_display_options_are_independent_and_order_independent(self):
        from itertools import permutations

        for source_flag in ("--source", "-s"):
            for lineinfo, source in (
                (False, False),
                (True, False),
                (False, True),
                (True, True),
            ):
                groups = [["--sass", __file__]]
                if lineinfo:
                    groups.append(["--lineinfo"])
                if source:
                    groups.append([source_flag])
                for ordered in permutations(groups):
                    argv = [arg for group in ordered for arg in group]
                    with self.subTest(argv=argv):
                        args = msobjdump_main.parse_args(argv)
                        self.assertEqual(args.lineinfo, lineinfo)
                        self.assertEqual(args.source, source)
                        with tempfile.TemporaryDirectory() as directory:
                            args.out_dir = directory
                            objdump = msobjdump_main.ObjDump(args)
                            try:
                                self.assertEqual(objdump.sass_lineinfo, lineinfo)
                                self.assertEqual(objdump.sass_source, source)
                            finally:
                                objdump._clean()

    def test_sass_modifiers_require_sass_and_suggest_help(self):
        for argv in (
            ["--lineinfo"],
            ["--source"],
            ["-s"],
            ["--source", "--lineinfo"],
            ["--dump-elf", __file__, "--lineinfo"],
        ):
            with (
                self.subTest(argv=argv),
                contextlib.redirect_stderr(StringIO()) as error,
            ):
                with self.assertRaises(SystemExit) as raised:
                    msobjdump_main.parse_args(argv)
                self.assertEqual(raised.exception.code, 2)
                self.assertIn("require --sass", error.getvalue())
                self.assertIn("--help", error.getvalue())

    def test_sass_display_options_preserve_existing_cli(self):
        args = msobjdump_main.parse_args(["-l", __file__])
        self.assertEqual(args.list_elf, os.path.realpath(__file__))
        for alias in ("-g", "--line-info"):
            with self.subTest(alias=alias), contextlib.redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    msobjdump_main.parse_args(["--sass", __file__, alias])
                self.assertEqual(raised.exception.code, 2)
        for conflicting in (
            ["--dump-elf", __file__],
            ["--extract-elf", __file__],
            ["--list-elf", __file__],
            ["--verbose"],
            ["--out-dir", "."],
        ):
            with (
                self.subTest(conflicting=conflicting),
                contextlib.redirect_stderr(StringIO()),
            ):
                with self.assertRaises(SystemExit) as raised:
                    msobjdump_main.parse_args(
                        ["--sass", __file__, "--source", *conflicting]
                    )
                self.assertEqual(raised.exception.code, 2)

    def test_sass_help_describes_dependencies(self):
        with contextlib.redirect_stdout(StringIO()) as output:
            with self.assertRaises(SystemExit) as raised:
                msobjdump_main.parse_args(["--help"])
        self.assertEqual(raised.exception.code, 0)
        help_text = " ".join(output.getvalue().split())
        for text in (
            "--lineinfo",
            "--source",
            "-s",
            "Requires --sass",
        ):
            self.assertIn(text, help_text)

    def test_sass_command_options_keep_paths_as_arguments(self):
        for lineinfo, source in (
            (False, False),
            (True, False),
            (False, True),
            (True, True),
        ):
            with (
                self.subTest(lineinfo=lineinfo, source=source),
                patch.object(utils.subprocess, "run") as run,
            ):
                utils.disassemble_with_llvm_objdump(
                    "目录/device with spaces.o",
                    "/tool path/llvm-objdump",
                    lineinfo=lineinfo,
                    source=source,
                )
                expected = ["/tool path/llvm-objdump", "-d"]
                if lineinfo:
                    expected.append("-l")
                if source:
                    expected.append("-S")
                expected.append("目录/device with spaces.o")
                self.assertEqual(run.call_args.args[0], expected)
                self.assertFalse(run.call_args.kwargs["shell"])

    def test_sass_annotations_and_warnings_preserved_for_each_image(self):
        for lineinfo, source in (
            (False, False),
            (True, False),
            (False, True),
            (True, True),
        ):
            with (
                self.subTest(lineinfo=lineinfo, source=source),
                tempfile.TemporaryDirectory() as directory,
            ):
                args = msobjdump_main.parse_args(["--sass", __file__])
                args.out_dir, args.lineinfo, args.source = directory, lineinfo, source
                objdump = msobjdump_main.ObjDump(args)
                images = [
                    msobjdump_main.SassImage("first.o", "archive(first.o)"),
                    msobjdump_main.SassImage("second.o", "archive(second.o)"),
                ]
                output = "目录/source file.cce:12\nint value = 1; // <unknown> in source\n  0: 00000000 END\n"
                warning = "llvm-objdump: warning: source file not found\n"
                try:
                    with (
                        patch.object(
                            utils,
                            "disassemble_with_llvm_objdump",
                            return_value=subprocess.CompletedProcess(
                                [], 0, output, warning
                            ),
                        ) as run,
                        contextlib.redirect_stdout(StringIO()) as out,
                        contextlib.redirect_stderr(StringIO()) as err,
                    ):
                        self.assertEqual(
                            objdump._report_sass_results(images, [], "llvm-objdump"), 0
                        )
                    self.assertEqual(run.call_count, 2)
                    for call, image in zip(run.call_args_list, images):
                        self.assertEqual(call.args, (image.path, "llvm-objdump"))
                        self.assertEqual(
                            call.kwargs, {"lineinfo": lineinfo, "source": source}
                        )
                        self.assertIn(image.source, out.getvalue())
                    self.assertEqual(out.getvalue().count(output), 2)
                    self.assertEqual(err.getvalue().count(warning.strip()), 2)
                finally:
                    objdump._clean()

    def test_sass_partial_failure_keeps_successful_image(self):
        with tempfile.TemporaryDirectory() as directory:
            args = msobjdump_main.parse_args(["--sass", __file__, "--source"])
            args.out_dir = directory
            objdump = msobjdump_main.ObjDump(args)
            try:
                images = [
                    msobjdump_main.SassImage("good.o", "good.o"),
                    msobjdump_main.SassImage("bad.o", "bad.o"),
                ]
                results = [
                    subprocess.CompletedProcess([], 0, "  0: 00000000 END\n", ""),
                    subprocess.CompletedProcess([], 1, "", "backend failure"),
                ]
                with (
                    patch.object(
                        utils, "disassemble_with_llvm_objdump", side_effect=results
                    ),
                    contextlib.redirect_stdout(StringIO()) as out,
                    contextlib.redirect_stderr(StringIO()) as err,
                ):
                    self.assertEqual(
                        objdump._report_sass_results(images, [], "llvm-objdump"), 1
                    )
                self.assertIn("good.o", out.getvalue())
                self.assertIn("END", out.getvalue())
                self.assertIn("bad.o", err.getvalue())
                self.assertIn("backend failure", err.getvalue())
            finally:
                objdump._clean()

    def test_classify_sass_file_recognizes_device_machine(self):
        objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        with tempfile.NamedTemporaryFile() as input_file:
            header = subprocess.CompletedProcess(
                [], 0, "Machine: <unknown>: 0x1029\n", ""
            )
            with (
                patch.object(utils, "get_elf_header", return_value=header),
                patch.object(
                    utils,
                    "get_section_headers_in_file",
                    return_value="[ 2] .text PROGBITS 00000000 000040 000008 00 AX",
                ),
            ):
                self.assertEqual(objdump._classify_sass_file(input_file.name), "device")

    def test_collect_sass_images_visits_all_archive_members(self):
        objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        objdump.tmp_dir = tempfile.mkdtemp()
        archive_file = os.path.join(objdump.tmp_dir, "libkernels.a")
        with open(archive_file, "wb") as file_obj:
            file_obj.write(msobjdump_main.ARCHIVE_MAGIC)

        def extract_member(_archive, _member, output_file):
            with open(output_file, "wb") as file_obj:
                file_obj.write(b"member")
            return subprocess.CompletedProcess([], 0, b"", b"")

        try:
            with (
                patch.object(
                    utils,
                    "list_archive_members",
                    return_value=subprocess.CompletedProcess(
                        [], 0, "host.o\ndevice.o\n", ""
                    ),
                ),
                patch.object(
                    utils, "extract_archive_member", side_effect=extract_member
                ),
                patch.object(
                    objdump,
                    "_classify_sass_file",
                    # 归档本身也经分类器分叉（统一分叉点）：archive -> host.o -> device.o
                    side_effect=["archive", "host", "device"],
                ),
            ):
                images, diagnostics = objdump._collect_sass_images(archive_file)

            self.assertEqual(diagnostics, [])
            self.assertEqual(len(images), 1)
            self.assertIn("device.o", images[0].source)
        finally:
            shutil.rmtree(objdump.tmp_dir)

    def test_collect_sass_images_preserves_archive_member_whitespace(self):
        with tempfile.TemporaryDirectory(dir=FILE_PATH) as temp_dir:
            for index, member_name in enumerate((" leading.o", "device.o ")):
                with self.subTest(member_name=member_name):
                    archive_file = os.path.join(temp_dir, f"libkernels_{index}.a")
                    member_path = os.path.join(temp_dir, member_name)
                    with open(member_path, "wb") as file_obj:
                        file_obj.write(member_name.encode("utf-8"))
                    subprocess.run(
                        ["ar", "rcs", archive_file, member_name],
                        cwd=temp_dir,
                        check=True,
                    )

                    objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
                    objdump.tmp_dir = tempfile.mkdtemp(dir=temp_dir)
                    try:
                        with patch.object(
                            objdump,
                            "_classify_sass_file",
                            side_effect=["archive", "device"],
                        ):
                            images, diagnostics = objdump._collect_sass_images(
                                archive_file
                            )

                        self.assertEqual(diagnostics, [])
                        self.assertEqual(len(images), 1)
                        with open(images[0].path, "rb") as file_obj:
                            self.assertEqual(
                                file_obj.read(), member_name.encode("utf-8")
                            )
                    finally:
                        shutil.rmtree(objdump.tmp_dir)

    def test_disassemble_sass_rejects_placeholder_output(self):
        objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        objdump.sass_lineinfo = False
        objdump.sass_source = False
        image = msobjdump_main.SassImage("device.o", "device.o")
        result = subprocess.CompletedProcess(
            [],
            0,
            "device.o: file format elf64-hiipu\n"
            "       0:                00000000 <not available>\n",
            "",
        )
        with patch.object(utils, "disassemble_with_llvm_objdump", return_value=result):
            output, error = objdump._disassemble_sass_image(image, "llvm-objdump")

        self.assertIn("<not available>", output)
        self.assertIn("no usable device instructions", error)

    def test_disassemble_sass_rejects_empty_output(self):
        objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        objdump.sass_lineinfo = False
        objdump.sass_source = False
        image = msobjdump_main.SassImage("device.o", "device.o")
        result = subprocess.CompletedProcess([], 0, "", "")
        with patch.object(utils, "disassemble_with_llvm_objdump", return_value=result):
            output, error = objdump._disassemble_sass_image(image, "llvm-objdump")

        self.assertEqual(output, "")
        self.assertIn("no valid instruction lines", error)

    def test_disassemble_sass_rejects_partial_unknown_output(self):
        objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        objdump.sass_lineinfo = False
        objdump.sass_source = False
        image = msobjdump_main.SassImage("device.o", "device.o")
        result = subprocess.CompletedProcess(
            [],
            0,
            "       0:                00000000\tEND\n       4:                00000000 <unknown>\n",
            "",
        )
        with patch.object(utils, "disassemble_with_llvm_objdump", return_value=result):
            output, error = objdump._disassemble_sass_image(image, "llvm-objdump")

        self.assertIn("END", output)
        self.assertIn("unknown instructions", error)

    def test_sass_process_prints_source_and_returns_success(self):
        objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        objdump.src_obj = __file__
        objdump._sass_tools = {}
        objdump.tmp_dir = tempfile.mkdtemp()
        image = msobjdump_main.SassImage("device.o", "input.o")
        try:
            with (
                patch.object(
                    objdump, "_get_sass_tool", return_value="/opt/llvm-objdump"
                ),
                patch.object(
                    objdump, "_collect_sass_images", return_value=([image], [])
                ),
                patch.object(
                    objdump,
                    "_disassemble_sass_image",
                    return_value=(
                        "device.o: file format elf64-hiipu\n"
                        "       0:                00000000\tEND\n",
                        "",
                    ),
                ),
                contextlib.redirect_stdout(StringIO()) as output,
            ):
                result = objdump._sass_process()

            self.assertEqual(result, 0)
            self.assertIn("===== [SASS] input.o =====", output.getvalue())
            self.assertIn("END", output.getvalue())
        finally:
            shutil.rmtree(objdump.tmp_dir)

    def test_constructor_cleans_temporary_directory_when_initialization_fails(self):
        with tempfile.TemporaryDirectory() as out_dir:
            args = SimpleNamespace(out_dir=out_dir)
            with patch.object(
                msobjdump_main.ObjDump,
                "_set_parse_obj_and_mode",
                side_effect=RuntimeError("parse failed"),
            ):
                with self.assertRaisesRegex(RuntimeError, "parse failed"):
                    msobjdump_main.ObjDump(args)

            self.assertEqual(
                [name for name in os.listdir(out_dir) if name.startswith("objdump_")],
                [],
            )

    def _make_out_dir(self, out_dir_name):
        out_dir = os.path.join(FILE_PATH, out_dir_name)
        if not os.path.exists(out_dir):
            os.mkdir(out_dir)
        return out_dir

    def _clean_out_dir(self, out_dir_name):
        out_dir = os.path.join(FILE_PATH, out_dir_name)
        if os.path.exists(out_dir):
            shutil.rmtree(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.get_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    def test_dump_elf_aclnn(
        self, mock_all, mock_section, mock_symbol, mock_elf_content
    ):
        mock_symbol.return_value = (
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_start\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_end\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_size\n"
        )
        mock_all.return_value = (
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_start\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_end\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_size\n"
        )
        mock_section.return_value = (
            "[21] .dynamic          DYNAMIC         00000000000dff10 0def10 000260 10  WA  4   0  8\n"
            "[22] .got              PROGBITS        00000000000e0170 0df170 001e90 08  WA  0   0  8\n"
            "[23] .data             PROGBITS        00000000000e2000 0e1000 0bb493 00  WA  0   0 32\n"
            "[24] .bss              NOBITS          000000000019d4a0 19c493 005ed0 00  WA  0   0 32\n"
            "[25] .comment          PROGBITS        0000000000000000 19c493 000045 01  MS  0   0  1"
        )
        mock_elf_content.return_value = bytes('{"test_key": "test_content"}\n', "utf-8")
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_dump_elf_aclnn")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_dump_elf_aclnn.o")
        parse_mock.dump_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.list_elf = None
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError as e:
            print(f"runtime error: {str(e)}")
            self.assertTrue(False)
        else:
            print("test run dump elf without exception")
            self.assertTrue(True)
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.get_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    def test_list_elf_aclnn(
        self, mock_all, mock_section, mock_symbol, mock_elf_content
    ):
        mock_symbol.return_value = (
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_start\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_end\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_size\n"
        )
        mock_all.return_value = (
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_start\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_end\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_size\n"
        )
        mock_section.return_value = (
            "[21] .dynamic          DYNAMIC         00000000000dff10 0def10 000260 10  WA  4   0  8\n"
            "[22] .got              PROGBITS        00000000000e0170 0df170 001e90 08  WA  0   0  8\n"
            "[23] .data             PROGBITS        00000000000e2000 0e1000 0bb493 00  WA  0   0 32\n"
            "[24] .bss              NOBITS          000000000019d4a0 19c493 005ed0 00  WA  0   0 32\n"
            "[25] .comment          PROGBITS        0000000000000000 19c493 000045 01  MS  0   0  1"
        )
        mock_elf_content.return_value = bytes('{"test_key": "test_content"}\n', "utf-8")
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_list_elf_aclnn")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_list_elf_aclnn.o")
        parse_mock.list_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.dump_elf = None
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError:
            self.assertTrue(False)
        else:
            print("test run dump elf without exception")
            self.assertTrue(True)
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.get_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    def test_extr_elf_aclnn(
        self, mock_all, mock_section, mock_symbol, mock_elf_content
    ):
        mock_symbol.return_value = (
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_start\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_end\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_size\n"
        )
        mock_all.return_value = (
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_start\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_end\n"
            "7824: 000000000010c0a0     0 NOTYPE  GLOBAL DEFAULT   23 _binary_ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4_o_size\n"
        )
        mock_section.return_value = (
            "[21] .dynamic          DYNAMIC         00000000000dff10 0def10 000260 10  WA  4   0  8\n"
            "[22] .got              PROGBITS        00000000000e0170 0df170 001e90 08  WA  0   0  8\n"
            "[23] .data             PROGBITS        00000000000e2000 0e1000 0bb493 00  WA  0   0 32\n"
            "[24] .bss              NOBITS          000000000019d4a0 19c493 005ed0 00  WA  0   0 32\n"
            "[25] .comment          PROGBITS        0000000000000000 19c493 000045 01  MS  0   0  1"
        )
        mock_elf_content.return_value = bytes('{"test_key": "test_content"}\n', "utf-8")
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_extr_elf_aclnn")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_extr_elf_aclnn")
        parse_mock.extr_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.list_elf = None
        parse_mock.dump_elf = None
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError:
            self.assertTrue(False)
            print("test file not exit")
        else:
            print("test run dump elf without exception")
            self.assertTrue(True)
        self.assertTrue(
            os.path.exists(
                os.path.join(
                    out_dir,
                    "ascend910b_axpy_custom_AxpyCustom_1b6d4d1dfddfa974458c4fc379248bc4.o",
                )
            )
        )
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.get_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    def test_dump_elf_ascend_kernel(
        self, mock_all, mock_section, mock_symbol, mock_elf_content
    ):
        mock_symbol.return_value = "test section list \n"
        mock_all.return_value = "test all section and symbols \n"
        mock_section.return_value = (
            "[22] .data             PROGBITS        000000000003f000 02f000 000010 00  WA  0   0  8\n"
            "[23] .ascend.kernel.ascend910b1.ascendc_kernels_npu PROGBITS   0000  0002  004  00  WA  0   0  8\n"
            "[24] .bss              NOBITS          00000000000bbe70 0abe6c 000128 00  WA  0   0  8\n"
        )
        mock_elf_content.return_value = b"\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00\x00\x00"
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_dump_elf_ascend_kernel")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_dump_elf_ascend_kernel.o")
        parse_mock.dump_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.list_elf = None
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError as e:
            print(f"runtime error: {str(e)}")
            self.assertTrue(False)
        else:
            print("test run dump elf without exception")
            self.assertTrue(True)
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.get_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    def test_list_elf_ascend_kernel(
        self, mock_all, mock_section, mock_symbol, mock_elf_content
    ):
        mock_symbol.return_value = "test section list \n"
        mock_all.return_value = "test all section and symbols \n"
        mock_section.return_value = (
            "[22] .data             PROGBITS        000000000003f000 02f000 000010 00  WA  0   0  8\n"
            "[23] .ascend.kernel.ascend910b1.ascendc_kernels_npu PROGBITS   0000  0002  004 00  WA  0   0  8\n"
            "[24] .bss              NOBITS          00000000000bbe70 0abe6c 000128 00  WA  0   0  8\n"
        )
        mock_elf_content.return_value = b"\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00\x00\x00"
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_list_elf_ascend_kernel")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_list_elf_ascend_kernel.o")
        parse_mock.list_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.dump_elf = None
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError:
            self.assertTrue(False)
        else:
            print("test run dump elf without exception")
            self.assertTrue(True)
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.get_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    def test_extr_elf_ascend_kernel(
        self, mock_all, mock_section, mock_symbol, mock_elf_content
    ):
        mock_symbol.return_value = "test section list \n"
        mock_all.return_value = "test all section and symbols \n"
        mock_section.return_value = (
            "[22] .data             PROGBITS        000000000003f000 02f000 000010 00  WA  0   0  8\n"
            "[23] .ascend.kernel.ascend910b1.ascendc_kernels_npu PROGBITS        0000  0002  004 00  WA  0   0  8\n"
            "[24] .bss              NOBITS          00000000000bbe70 0abe6c 000128 00  WA  0   0  8\n"
        )
        mock_elf_content.return_value = b"\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00\x00\x00"
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_extr_elf_ascend_kernel")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_extr_elf_ascend_kernel")
        parse_mock.extr_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.list_elf = None
        parse_mock.dump_elf = None
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError:
            self.assertTrue(False)
            print("test file not exit")
        else:
            print("test run dump elf without exception")
            self.assertTrue(True)
        self.assertTrue(
            os.path.exists(
                os.path.join(out_dir, "ascend910b1_ascendc_kernels_npu_0_mix.o")
            )
        )
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.get_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    def test_dump_elf_ascend_meta(
        self, mock_all, mock_section, mock_symbol, mock_elf_content
    ):
        mock_symbol.return_value = "test section list \n"
        mock_all.return_value = "test all section and symbols \n"
        mock_section.return_value = (
            "[14] .debug_line_str   PROGBITS        0000000000000000 06ca46 0016c1 01  MS  0   0  1\n"
            "[15] .ascend.meta.gen_FFN_2000_mix_aiv NOTE            0000000000000000 06e108 000010 00      0   0  4\n"
            "[16] .bl_uninit        NOBITS          0000000000000000 06e118 000028 00      0   0  1\n"
        )
        mock_elf_content.return_value = (
            b"\x01\x00\x04\x00\x04\x00\x00\x00\x03\x00\x04\x00\x01\x00\x01\x00"
        )
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_dump_elf_ascend_meta")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_dump_elf_ascend_meta.o")
        parse_mock.dump_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.list_elf = None
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError as e:
            print(f"runtime error: {str(e)}")
            self.assertTrue(False)
        else:
            print("test run dump elf without exception")
            self.assertTrue(True)
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.get_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    def test_dump_elf_ascend_meta_with_early_start(
        self, mock_all, mock_section, mock_symbol, mock_elf_content
    ):
        mock_symbol.return_value = "test section list \n"
        mock_all.return_value = "test all section and symbols \n"
        mock_section.return_value = (
            "[15] .ascend.meta.gen_FFN_2000_mix_aic NOTE "
            "0000000000000000 06e108 000018 00      0   0  4\n"
        )
        mock_elf_content.return_value = (
            b"\x01\x00\x04\x00\x04\x00\x00\x00"
            b"\x03\x00\x04\x00\x01\x00\x00\x00"
            b"\x0b\x00\x04\x00\x01\x00\x00\x00"
        )
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_dump_elf_ascend_meta_early_start")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_dump_elf_ascend_meta_early_start.o")
        parse_mock.dump_elf = elf_file
        parse_mock.extr_elf = None
        parse_mock.list_elf = None
        with open(elf_file, "a+") as f:
            f.write("test")

        buf = StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError as e:
            print(f"runtime error: {str(e)}")
            self.assertTrue(False)
        else:
            print("test run dump elf without exception")
            output = buf.getvalue()
            self.assertIn("ENABLE_EARLY_START: 1", output)
            self.assertIn("KERNEL_TYPE: MIX_AIC_MAIN", output)
            self.assertIn("MIX_TASK_RATION: [1:0]", output)
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.extract_aicore_binary_from_elf")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_symbols_in_file")
    def test_dump_elf_fusion_compile(
        self, mock_symbol, mock_section, mock_all, mock_run, mock_elf_content
    ):
        mock_symbol.side_effect = ["test section list \n", "test section list \n"]
        mock_all.return_value = "test all section and symbols \n"
        mock_section.side_effect = [
            "[26] .aicore_binary    PROGBITS        00000000000ab500 07b500 0072c0 00  WA  0   0 256\n",
            "[15] .ascend.meta.gen_FFN_2000_mix_aiv NOTE            0000000000000000 06e108 000010 00      0   0  4\n",
            "[15] .ascend.meta.gen_FFN_2000_mix_aiv NOTE            0000000000000000 06e108 000010 00      0   0  4\n",
        ]
        mock_elf_content.return_value = (
            b"\x01\x00\x04\x00\x04\x00\x00\x00\x03\x00\x04\x00\x01\x00\x01\x00"
        )

        def _mock_objcopy(input_file, output_file):
            with open(output_file, "wb") as f:
                f.write(b"fusion_meta")
            result = MagicMock()
            result.returncode = 0
            result.stderr = ""
            return result

        mock_run.side_effect = _mock_objcopy
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_dump_elf_fusion_compile")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_dump_elf_fusion_compile")
        parse_mock.dump_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.list_elf = None
        parse_mock.verbose = True
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError:
            self.assertTrue(False)
        else:
            self.assertTrue(mock_run.called)
            self.assertTrue(mock_all.called)
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.extract_aicore_binary_from_elf")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_symbols_in_file")
    def test_list_elf_fusion_compile(
        self, mock_symbol, mock_section, mock_all, mock_run, mock_elf_content
    ):
        mock_symbol.side_effect = ["test section list \n", "test section list \n"]
        mock_all.return_value = "test all section and symbols \n"
        kernel_section = "[23] .ascend.kernel.ascend910b1.ascendc_kernels_npu PROGBITS   0000  0002  018 00  WA  0   0  8\n"
        mock_section.side_effect = [
            "[26] .aicore_binary    PROGBITS        00000000000ab500 07b500 0072c0 00  WA  0   0 256\n",
            kernel_section,
            kernel_section,
        ]
        mock_elf_content.return_value = b"\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00"

        def _mock_objcopy(input_file, output_file):
            with open(output_file, "wb") as f:
                f.write(b"fusion_kernel")
            result = MagicMock()
            result.returncode = 0
            result.stderr = ""
            return result

        mock_run.side_effect = _mock_objcopy
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_list_elf_fusion_compile")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_list_elf_fusion_compile")
        parse_mock.list_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.dump_elf = None
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError:
            self.assertTrue(False)
        else:
            self.assertTrue(mock_run.called)
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.extract_aicore_binary_from_elf")
    @patch("msobjdump.utils.get_all_section_symbols_in_file")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_symbols_in_file")
    def test_extract_elf_fusion_compile(
        self, mock_symbol, mock_section, mock_all, mock_run, mock_elf_content
    ):
        mock_symbol.side_effect = ["test section list \n", "test section list \n"]
        mock_all.return_value = "test all section and symbols \n"
        kernel_section = "[23] .ascend.kernel.ascend910b1.ascendc_kernels_npu PROGBITS   0000  0002  018 00  WA  0   0  8\n"
        mock_section.side_effect = [
            "[26] .aicore_binary    PROGBITS        00000000000ab500 07b500 0072c0 00  WA  0   0 256\n",
            kernel_section,
            kernel_section,
        ]
        mock_elf_content.return_value = b"\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00"

        def _mock_objcopy(input_file, output_file):
            with open(output_file, "wb") as f:
                f.write(b"fusion_kernel")
            result = MagicMock()
            result.returncode = 0
            result.stderr = ""
            return result

        mock_run.side_effect = _mock_objcopy
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_extract_elf_fusion_compile")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_extract_elf_fusion_compile")
        parse_mock.extr_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.list_elf = None
        parse_mock.dump_elf = None
        try:
            msobjdump_main.run_obj_dump(parse_mock)
        except RuntimeError:
            self.assertTrue(False)
        else:
            self.assertTrue(mock_run.called)
        self.assertTrue(
            os.path.exists(
                os.path.join(out_dir, "test_extract_elf_fusion_compile.aicore.o")
            )
        )
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.extract_aicore_binary_from_elf")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_symbols_in_file")
    def test_list_elf_fusion_compile_meta_lists_extracted_binary(
        self, mock_symbol, mock_section, mock_run, mock_elf_content
    ):
        mock_symbol.side_effect = ["test section list \n", "test section list \n"]
        mock_section.side_effect = [
            "[26] .aicore_binary    PROGBITS        00000000000ab500 07b500 0072c0 00  WA  0   0 256\n",
            "[15] .ascend.meta.gen_FFN_2000_mix_aiv NOTE            0000000000000000 06e108 000010 00      0   0  4\n",
        ]
        mock_elf_content.return_value = (
            b"\x01\x00\x04\x00\x04\x00\x00\x00\x03\x00\x04\x00\x01\x00\x01\x00"
        )

        def _mock_objcopy(input_file, output_file):
            with open(output_file, "wb") as f:
                f.write(b"fusion_meta")
            result = MagicMock()
            result.returncode = 0
            result.stderr = ""
            return result

        mock_run.side_effect = _mock_objcopy
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_list_elf_fusion_compile_meta")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_list_elf_fusion_compile_meta")
        parse_mock.list_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.dump_elf = None

        captured_output = StringIO()
        with contextlib.redirect_stdout(captured_output):
            msobjdump_main.run_obj_dump(parse_mock)
        self.assertIn(
            "ELF file    0: test_list_elf_fusion_compile_meta.aicore.o",
            captured_output.getvalue(),
        )
        self._clean_out_dir(out_dir)

    @patch("msobjdump.msobjdump_main.ObjDump._get_segment_content")
    @patch("msobjdump.utils.extract_aicore_binary_from_elf")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_symbols_in_file")
    def test_extract_elf_fusion_compile_meta_outputs_extracted_binary(
        self, mock_symbol, mock_section, mock_run, mock_elf_content
    ):
        mock_symbol.side_effect = ["test section list \n", "test section list \n"]
        mock_section.side_effect = [
            "[26] .aicore_binary    PROGBITS        00000000000ab500 07b500 0072c0 00  WA  0   0 256\n",
            "[15] .ascend.meta.gen_FFN_2000_mix_aiv NOTE            0000000000000000 06e108 000010 00      0   0  4\n",
        ]
        mock_elf_content.return_value = (
            b"\x01\x00\x04\x00\x04\x00\x00\x00\x03\x00\x04\x00\x01\x00\x01\x00"
        )

        def _mock_objcopy(input_file, output_file):
            with open(output_file, "wb") as f:
                f.write(b"fusion_meta")
            result = MagicMock()
            result.returncode = 0
            result.stderr = ""
            return result

        mock_run.side_effect = _mock_objcopy
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_extract_elf_fusion_compile_meta")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_extract_elf_fusion_compile_meta")
        parse_mock.extr_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.list_elf = None
        parse_mock.dump_elf = None

        msobjdump_main.run_obj_dump(parse_mock)
        self.assertTrue(
            os.path.exists(
                os.path.join(out_dir, "test_extract_elf_fusion_compile_meta.aicore.o")
            )
        )
        self._clean_out_dir(out_dir)

    @patch("msobjdump.utils.extract_aicore_binary_from_elf")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_symbols_in_file")
    def test_fusion_compile_objcopy_failed(self, mock_symbol, mock_section, mock_run):
        mock_symbol.return_value = "test section list \n"
        mock_section.return_value = "[26] .aicore_binary    PROGBITS        00000000000ab500 07b500 0072c0 00  WA  0   0 256\n"
        result = MagicMock()
        result.returncode = 1
        result.stderr = "mock objcopy failed"
        mock_run.return_value = result

        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_fusion_compile_objcopy_failed")
        parse_mock.out_dir = out_dir
        elf_file = os.path.join(out_dir, "test_fusion_compile_objcopy_failed")
        parse_mock.dump_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.list_elf = None

        with self.assertRaises(RuntimeError):
            msobjdump_main.run_obj_dump(parse_mock)
        self._clean_out_dir(out_dir)

    @patch("msobjdump.utils.extract_aicore_binary_from_elf")
    @patch("msobjdump.utils.get_section_headers_in_file")
    @patch("msobjdump.utils.get_symbols_in_file")
    def test_fusion_compile_unsupported_after_extract(
        self, mock_symbol, mock_section, mock_run
    ):
        mock_symbol.side_effect = ["test section list \n", "test section list \n"]
        mock_section.side_effect = [
            "[26] .aicore_binary    PROGBITS        00000000000ab500 07b500 0072c0 00  WA  0   0 256\n",
            "[26] .aicore_binary    PROGBITS        00000000000ab500 07b500 0072c0 00  WA  0   0 256\n",
        ]

        def _mock_objcopy(input_file, output_file):
            with open(output_file, "wb") as f:
                f.write(b"fusion_unknown")
            result = MagicMock()
            result.returncode = 0
            result.stderr = ""
            return result

        mock_run.side_effect = _mock_objcopy
        parse_mock = MagicMock()
        out_dir = self._make_out_dir("test_fusion_compile_unsupported_after_extract")
        parse_mock.out_dir = out_dir
        parse_mock.verbose = False
        elf_file = os.path.join(
            out_dir, "test_fusion_compile_unsupported_after_extract"
        )
        parse_mock.dump_elf = elf_file
        with open(elf_file, "a+") as f:
            f.write("test")
        parse_mock.extr_elf = None
        parse_mock.list_elf = None

        msobjdump_main.run_obj_dump(parse_mock)
        self._clean_out_dir(out_dir)

    def test_extract_aicore_binary_edge_cases(self):
        with self.subTest("remove old tmp and extract success"):
            objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
            objdump.tmp_dir = self._make_out_dir(
                "test_extract_aicore_binary_edge_success"
            )
            objdump.obj = os.path.join(objdump.tmp_dir, "input_bin")
            with (
                patch(
                    "msobjdump.msobjdump_main.os.path.exists", side_effect=[True, True]
                ) as mock_exists,
                patch("msobjdump.msobjdump_main.os.path.getsize", return_value=8),
                patch("msobjdump.msobjdump_main.os.remove") as mock_remove,
                patch(
                    "msobjdump.msobjdump_main.utils.extract_aicore_binary_from_elf"
                ) as mock_run,
            ):
                result = MagicMock()
                result.returncode = 0
                result.stderr = ""
                mock_run.return_value = result
                tmp_file = objdump._extract_aicore_binary()
                self.assertTrue(mock_exists.called)
                self.assertTrue(mock_remove.called)
                self.assertEqual(
                    tmp_file,
                    os.path.join(objdump.tmp_dir, "fusion_aicore_binary.aicore.o"),
                )
            shutil.rmtree(objdump.tmp_dir, ignore_errors=True)

        with self.subTest("llvm-objcopy not found"):
            objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
            objdump.tmp_dir = self._make_out_dir(
                "test_extract_aicore_binary_edge_not_found"
            )
            objdump.obj = os.path.join(objdump.tmp_dir, "input_bin")
            with (
                patch("msobjdump.msobjdump_main.os.path.exists", return_value=False),
                patch(
                    "msobjdump.msobjdump_main.utils.extract_aicore_binary_from_elf",
                    side_effect=FileNotFoundError("llvm-objcopy not found"),
                ),
            ):
                with self.assertRaises(RuntimeError) as ctx:
                    objdump._extract_aicore_binary()
                self.assertIn("llvm-objcopy is not available", str(ctx.exception))
            shutil.rmtree(objdump.tmp_dir, ignore_errors=True)

        with self.subTest("extract output missing or empty"):
            objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
            objdump.tmp_dir = self._make_out_dir(
                "test_extract_aicore_binary_edge_empty"
            )
            objdump.obj = os.path.join(objdump.tmp_dir, "input_bin")
            with (
                patch(
                    "msobjdump.msobjdump_main.os.path.exists",
                    side_effect=[False, False],
                ),
                patch(
                    "msobjdump.msobjdump_main.utils.extract_aicore_binary_from_elf"
                ) as mock_run,
            ):
                result = MagicMock()
                result.returncode = 0
                result.stderr = ""
                mock_run.return_value = result
                with self.assertRaises(RuntimeError) as ctx:
                    objdump._extract_aicore_binary()
                self.assertIn("file is empty or missing", str(ctx.exception))
            shutil.rmtree(objdump.tmp_dir, ignore_errors=True)

    def test_show_ascend_meta_tlv_block_num(self):
        content = b"\xff\xff\xff\xff"
        t = 15
        length = 4
        index = 0

        captured_output = StringIO()
        sys.stdout = captured_output

        msobjdump_main.ObjDump._show_ascend_meta_tlv(content, t, length, index)

        sys.stdout = sys.__stdout__
        output = captured_output.getvalue().strip()

        self.assertEqual(output, "BLOCK_NUM: 0xFFFFFFFF")

    def test_show_ascend_meta_op_tlv_dedup_for_aicore_binary(self):
        objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        objdump.obj_type = msobjdump_main.ObjType.TYPE_AICORE_BINARY
        objdump._aicore_binary_meta_printed = set()
        version_content = b"\x01\x00\x00\x00"
        runtime_content = b"\x03\x00\x00\x00"

        captured_output = StringIO()
        sys.stdout = captured_output

        objdump._show_ascend_meta_op_tlv(
            runtime_content, msobjdump_main.B_TYPE_RUNTIME_IMPLICIT_INFO, 4, 0
        )
        objdump._show_ascend_meta_op_tlv(
            version_content, msobjdump_main.B_TYPE_VERSION, 4, 0
        )
        objdump._show_ascend_meta_op_tlv(
            version_content, msobjdump_main.B_TYPE_VERSION, 4, 0
        )

        sys.stdout = sys.__stdout__
        output = captured_output.getvalue().strip().splitlines()

        self.assertEqual(
            output, ["RUNTIME_IMPLICIT_INFO: L2Cache Hint Flag", "VERSION: 1"]
        )

    def test_show_ascend_meta_op_tlv_other_types(self):
        objdump = msobjdump_main.ObjDump.__new__(msobjdump_main.ObjDump)
        objdump.obj_type = msobjdump_main.ObjType.TYPE_ASCEND_META
        objdump._aicore_binary_meta_printed = set()

        captured_output = StringIO()
        sys.stdout = captured_output

        objdump._show_ascend_meta_op_tlv(
            b"\x08\x00\x00\x00\x02\x00\x00\x00", msobjdump_main.B_TYPE_DEBUG, 8, 0
        )
        objdump._show_ascend_meta_op_tlv(
            b"\x00\x00\x03\x00", msobjdump_main.B_TYPE_DYNAMIC_PARAM, 4, 0
        )
        objdump._show_ascend_meta_op_tlv(
            b"\x01\x00\x02\x00", msobjdump_main.B_TYPE_OPTIONAL_PARAM, 4, 0
        )
        objdump._show_ascend_meta_op_tlv(
            b"\x03\x00\x00\x00", msobjdump_main.B_TYPE_RUNTIME_IMPLICIT_INFO, 4, 0
        )

        sys.stdout = sys.__stdout__
        output = captured_output.getvalue().strip().splitlines()

        self.assertEqual(
            output,
            [
                "DEBUG: debugBufSize=8, debugOptions=2",
                "DYNAMIC_PARAM: dynamicParamMode=3",
                "OPTIONAL_PARAM: optionalInputMode=1, optionalOutputMode=2",
                "RUNTIME_IMPLICIT_INFO: L2Cache Hint Flag",
            ],
        )

    def test_utils_helper_functions(self):
        self.assertEqual(
            utils.split_str_with_space("  foo\t bar   baz  "), ["foo", "bar", "baz"]
        )
        self.assertEqual(
            utils.get_str_between("_binary_demo_o_start", "_binary_", "_start"),
            "demo_o",
        )
        self.assertEqual(utils.get_str_between("abc", "_binary_", "_start"), "")
        self.assertTrue(utils.is_prefix_substring("Ascend910", ["config", "ascend"]))
        self.assertFalse(utils.is_prefix_substring("kernel", ["config", "ascend"]))

    @patch("msobjdump.utils.subprocess.run")
    @patch("msobjdump.utils.shutil.copy")
    @patch("msobjdump.utils.os.makedirs")
    @patch("msobjdump.utils.os.path.exists")
    def test_utils_file_and_command_wrappers(
        self, mock_exists, mock_makedirs, mock_copy, mock_run
    ):
        mock_exists.side_effect = [True, False]
        utils.copy_file_src_exist("/tmp/src.txt", "/tmp/out/dest.txt")
        mock_makedirs.assert_called_once_with("/tmp/out")
        mock_copy.assert_called_once_with("/tmp/src.txt", "/tmp/out/dest.txt")

        mock_exists.reset_mock()
        mock_makedirs.reset_mock()
        mock_copy.reset_mock()
        mock_exists.side_effect = None
        mock_exists.return_value = False
        utils.copy_file_src_exist("/tmp/missing.txt", "/tmp/out/dest.txt")
        mock_makedirs.assert_not_called()
        mock_copy.assert_not_called()

        mock_run.return_value = MagicMock(stdout="test output")
        self.assertEqual(utils.get_section_headers_in_file("test.o"), "test output")
        self.assertEqual(utils.get_symbols_in_file("test.o"), "test output")
        self.assertEqual(utils.get_all_section_symbols_in_file("test.o"), "test output")

        mock_result = MagicMock()
        mock_run.return_value = mock_result
        self.assertIs(utils.get_o_file_from_a_file("libtest.a", "demo.o"), mock_result)
        self.assertIs(
            utils.extract_aicore_binary_from_elf("demo", "demo.aicore.o"), mock_result
        )

    @patch("msobjdump.utils.subprocess.run")
    def test_utils_sass_wrappers_use_argument_lists_and_stable_locale(self, mock_run):
        mock_run.return_value = MagicMock(stdout="", stderr="", returncode=0)

        utils.get_elf_header("input with space.o")
        utils.list_archive_members("lib with space.a")
        utils.disassemble_with_llvm_objdump(
            "device with space.o", "/opt/cann/bin/llvm-objdump"
        )

        self.assertEqual(mock_run.call_count, 3)
        expected_commands = [
            ["readelf", "-hW", "input with space.o"],
            ["ar", "t", "lib with space.a"],
            ["/opt/cann/bin/llvm-objdump", "-d", "device with space.o"],
        ]
        for call, command in zip(mock_run.call_args_list, expected_commands):
            self.assertEqual(call.args[0], command)
            self.assertFalse(call.kwargs["shell"])
            self.assertEqual(call.kwargs["env"]["LC_ALL"], "C")
            self.assertEqual(call.kwargs["env"]["LANG"], "C")


if __name__ == "__main__":
    unittest.main()
