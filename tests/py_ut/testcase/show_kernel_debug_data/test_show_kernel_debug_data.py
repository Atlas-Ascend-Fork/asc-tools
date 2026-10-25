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
import tempfile
import unittest
import math
import logging
import importlib
from unittest.mock import MagicMock, patch, call, ANY

THIS_FILE_NAME = __file__
FILE_PATH = os.path.dirname(os.path.realpath(THIS_FILE_NAME))
ASCENDUMP_PATH = os.path.join(FILE_PATH, "../../../../", "utils/show_kernel_debug_data")
print(ASCENDUMP_PATH)
sys.path = [ASCENDUMP_PATH] + sys.path
show_kernel_debug_data_package = importlib.import_module("show_kernel_debug_data")
dump_logger = importlib.import_module("show_kernel_debug_data.dump_logger")
dump_parser = importlib.import_module("show_kernel_debug_data.dump_parser")
data_converter = importlib.import_module("show_kernel_debug_data.data_converter")
show_kernel_debug_data_api = show_kernel_debug_data_package.show_kernel_debug_data

TLV = dump_parser.TLV
DumpMessageHeader = dump_parser.DumpMessageHeader
ShapeInfo = dump_parser.ShapeInfo
MetaInfo = dump_parser.MetaInfo
TimeStampInfo = dump_parser.TimeStampInfo
FifoTimeStampInfo = dump_parser.FifoTimeStampInfo
DumpTensor = dump_parser.DumpTensor
FifoDumpTensor = dump_parser.FifoDumpTensor
PrintStruct = dump_parser.PrintStruct
FifoPrintStruct = dump_parser.FifoPrintStruct
FifoSimtPrintStruct = dump_parser.FifoSimtPrintStruct
BlockInfo = dump_parser.BlockInfo
FifoBlockInfo = dump_parser.FifoBlockInfo
DumpCoreContent = dump_parser.DumpCoreContent
FifoDumpCoreContent = dump_parser.FifoDumpCoreContent
DumpType = dump_parser.DumpType
DumpBinFile = dump_parser.DumpBinFile
FifoDumpBinFile = dump_parser.FifoDumpBinFile
get_enum_member_name = dump_parser.get_enum_member_name
TimeStampId = dump_parser.TimeStampId


class TestAscendump(unittest.TestCase):
    def setUp(self):
        # 每个测试用例执行之前做操作
        print("---------------------set up case----------------------------------")
        self.test_dir = tempfile.mkdtemp()
        self.logger = dump_parser.DUMP_PARSER_LOG.logger
        self.original_handlers = list(self.logger.handlers)
        for handler in self.original_handlers:
            if isinstance(handler, logging.FileHandler):
                self.logger.removeHandler(handler)
        dump_parser.DUMP_PARSER_LOG.set_log_file(
            os.path.join(self.test_dir, "parser.log")
        )

    def tearDown(self):
        # 每个测试用例执行之后做操作
        print("---------------------tear down case-------------------------------")
        for handler in list(self.logger.handlers):
            if handler not in self.original_handlers:
                self.logger.removeHandler(handler)
                if isinstance(handler, logging.FileHandler):
                    handler.close()
        for handler in self.original_handlers:
            if handler not in self.logger.handlers:
                self.logger.addHandler(handler)
        if os.path.exists(self.test_dir):
            shutil.rmtree(self.test_dir)

    def _make_out_dir(self, out_dir_name):
        out_dir = os.path.join(FILE_PATH, out_dir_name)
        if not os.path.exists(out_dir):
            os.mkdir(out_dir)
        return out_dir

    def _clean_out_dir(self, out_dir_name):
        out_dir = os.path.join(FILE_PATH, out_dir_name)
        if os.path.exists(out_dir):
            shutil.rmtree(out_dir)

    def test_ascendump_log_file(self):
        output_path = self._make_out_dir("test_ascendump_log_file")
        asd_logger = dump_logger.DumpParserLog()
        asd_logger.set_log_file(
            os.path.join(os.path.abspath(output_path), "test_ascendump_log_file.log")
        )
        self.assertTrue(
            os.path.join(os.path.abspath(output_path), "test_ascendump_log_file.log"),
            asd_logger.get_log_file,
        )
        self._clean_out_dir(output_path)

    def test_ascendump_parser_file(self):
        output_path = self._make_out_dir("test_ascendump_parser_file")
        test_dump_file = os.path.join(output_path, "test_dump_file.bin")
        with open(test_dump_file, "wb+") as f:
            f.seek(0)
            content = b"\x00"
            f.write(content)
        with (
            patch("sys.argv", ["show_kernel_debug_data", "./xx.bin", "./out"]),
            patch("sys.exit"),
            self.assertRaises(RuntimeError),
        ):
            dump_parser.execute_parse()
        auto_create_out = os.path.join(output_path, "auto_create_out")
        with (
            patch(
                "sys.argv", ["show_kernel_debug_data", test_dump_file, auto_create_out]
            ),
            patch("sys.exit"),
        ):
            dump_parser.execute_parse()
        self.assertTrue(os.path.isdir(auto_create_out))

        args = ["show_kernel_debug_data", "-h"]
        with (
            patch("sys.argv", args),
            patch("sys.exit"),
            patch("show_kernel_debug_data.dump_parser.parse_dump_bin"),
        ):
            dump_parser.execute_parse()
            dump_parser.parse_dump_bin.assert_not_called()
        self._clean_out_dir(output_path)

    @patch("show_kernel_debug_data.dump_parser.DumpBinFile.show_print")
    @patch("show_kernel_debug_data.dump_parser.get_install_path")
    def test_parse_dump_shape(self, get_install_path_mock, show_mock):
        get_install_path_mock.return_value = "/cann"
        data_path = self._make_out_dir("test_parse_dump_shape")
        test_dump_file = os.path.join(data_path, "test_dump_file.bin")
        with open(test_dump_file, "wb+") as f:
            f.seek(0)
            content = b"\x00\x00\x10\x00\x00\x00\x00\x00 \x00\x00\x00@\xf9\x0f\x00\xcd\xbc\xa5Z\x00\x00\x00\x00\xc0\x06 A\xc0\x12\x00\x00\x05\x00\x00\x00\x08\x00\x00\x00 \x00\x02\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00 \x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00,N\x01Y\x10W\nV\xa0V\rXbW\xfeW\xc4S<VgX4X\xd1L\xd2T\x0cX\tL\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00`\x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x16Y\xe1V\xe4U\xa9T\xe4R,W\tXEXWTVW\x17V\xecX!YXVjWZY\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00 \x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x82U,UXW2X(V\x16W+T!X\xb8U\x82VQU\xc5T8T\x9cX\xaaY\xf6Q\x18VfV\xd4V\x7fU\xe2V\xedVNVVU\x0eQ1L\xc9T\x83W\xccV\xa6KpX\x18W\x02\x00\x00\x00X\x00\x00\x00`\x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\xf0X\xc6U\x9bUjTjR@X\x1aV(W\x97R^W\x9bX\xb6W5N\xf6T^W\xe2U\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00 \x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00RU\xa6WKV\nZxPwU:VyU\xa1T\x01X\x9eT\xdcU\xdcU\xdeVMV\x88U\xd2UZQdX\xcfT\xd9UNU&W\x17W\x84WlX\x1aV\x9fUIX\xbcU\xdaV\x12U\x02\x00\x00\x00X\x00\x00\x00`\x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01U\xd6S&T\xc4V\xbaW\xf6V\xa5X\xabU\xebVnU?V\x94X\xf0V\xaaR\xe5P\x08X\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00 \x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x19U\x80X\xe6V\xecM\xdaV\x80U\x88O\x17Y\xf7V\xceU\xabQDO<Y1W\xceU\x13V$WkT\x85V\xaaVlU\xe4XCR(X\x88TxT{T\x89U\xa8T9Y\xb2Y8X\x02\x00\x00\x00X\x00\x00\x00`\x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00ST\xd8X\xb3Q\x00X\x00Z(U\xfaW&X\x1eX$R\xa1X\x1eX\xccX\x8eSTV\x08T\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00 \x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x14YjT\x06V\x8bR\xc0X\x1fY ZsU\x12YLKdX\nXLX\xe1T'U\tT\xfdX\xaaY\xd7V\xd2Y8X\xd9VrXLV\x90V\x90XuP\x7fX>U\x18U7V\x19Y\x02\x00\x00\x00X\x00\x00\x00`\x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x1eU\xe8X\xe2U\xe4V\x0bM*Y\x7fV`X\x0eX\xa6W\xe1XKY\"XTW\x8cU#M\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00 \x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\xb6T[U\xa5X\xceX>Q\xa9X\xebT*T-U\x04Y\xfaU\x07V\xcaWnF\xcaV&TZV Y\xe5S\xb2U\x88YAQdVHQKY\xd8W\xb0XdU\xe5L\x06V\xe2P\xb2T\x02\x00\x00\x00X\x00\x00\x00`\x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\xe8V\x0bT\xa8R\xa2Y\xeaOMVIU\x88X\xd8R\x84S\x8eW\x1cSYV~Y:RlL\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00 \x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\xfbU\x1cV\xaeX#U\xf4VAT\x0cT\x02Y\xb5G\xf0X\xc0V\x04VcR\x00X U\x9fN\xfbU\x86PVY\x06Z\x84TTT\x9cYWVdW\x8fWaTxV\x11W\xbeV\xd0TjU\x02\x00\x00\x00X\x00\x00\x00`\x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x93X\x01U)M?R\xe7W\x8dV|L\xaeX\xbfU\x98Y\xdcU*Y\xb0X\xecS\x93T\xa5X\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00 \x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x1eW\x86UiSXV\xb5VaU\x06M\x80W\xb8X\xf2U,W\xbaQ^VbX\x92X\x94T\xebXdX8U\xd6U2Y\xe6V2X'X\x80U0X\xc5RqV\xe2V7SvT2R\x02\x00\x00\x00X\x00\x00\x00`\x01\x00\x00\x01\x00\x00\x00\r\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x08X\xa0X\xc2W\xfeV\xc8RBT\xaaQ\xe5UBU\x82V^X\xdcW\x98U\x7fOhX\x88R\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x00(\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00X\x00\x00\x00\x00`\x02\xc0\x01\x00\x00\x00\x9a\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00 V\xacT\x04Y\xb1Y\xc0X<W\xe7SYO\xdcW)W\"X[WQXFV\x92Q\xbfX,N\x01Y\x10W\nV\xa0V\rXbW\xfeW\xc4S<VgX4X\xd1L\xd2T\x0cX\tL\x00"
            f.write(content)

        with patch("sys.argv", ["show_kernel_debug_data", test_dump_file, data_path]):
            dump_parser.parse_dump_bin(test_dump_file, data_path)
            show_mock.assert_called_once()
        self.assertTrue(os.path.exists(test_dump_file))
        self._clean_out_dir(data_path)

    @patch("show_kernel_debug_data.dump_parser.DumpBinFile.show_print")
    @patch("show_kernel_debug_data.dump_parser.get_install_path")
    def test_parse_time_stamp(self, get_install_path_mock, show_mock):
        get_install_path_mock.return_value = "/cann"
        data_path = self._make_out_dir("test_parse_time_stamp")
        test_dump_file = os.path.join(data_path, "test_dump_file.bin")
        with open(test_dump_file, "wb+") as f:
            f.write(b"\x00" * 1024 * 1024)
        with open(test_dump_file, "r+b") as f:
            f.seek(0)
            content = b"\x00\x00\x10\x00\x00\x00\x00\x00\x01\x00\x00\x00\xf0\xfe\x0f\x00\xcd\xbc\xa5Z\x00\x00\x00\x00\x10\x01 A\xc0\x12\x00\x00\x05\x00\x00\x00\x08\x00\x00\x00\x01\x00\x02\x00\x00\x00\x00\x00\x06\x00\x00\x00\x18\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xe0,P?\xb77\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x18\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00,-P?\xb77\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x18\x00\x00\x000\x00\x00\x00\x00\x00\x00\x00=-P?\xb77\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x18\x00\x00\x00\xd0\x07\x00\x00\x00\x00\x00\x00A-P?\xb77\x00\x00\x80\x03\x00\x00@\x12\x00\x00\x06\x00\x00\x00\x18\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00R-P?\xb77\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x18\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00[-P?\xb77\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x18\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00h-P?\xb77\x00\x00\x00\x00"
            f.write(content)

        args = ["show_kernel_debug_data", test_dump_file, data_path]
        with patch("sys.argv", args), patch("sys.exit"):
            dump_parser.parse_dump_bin(test_dump_file, data_path)
            show_mock.assert_called_once()
        self.assertTrue(os.path.exists(test_dump_file))

    @patch("show_kernel_debug_data.dump_parser.DumpBinFile.show_print")
    @patch("show_kernel_debug_data.dump_parser.get_install_path")
    @patch("glob.glob")
    @patch("subprocess.run")
    def test_pre_process(self, run_mock, glob_mock, get_install_path_mock, show_mock):
        get_install_path_mock.return_value = "/cann"
        data_path = self._make_out_dir("test_parse_time_stamp")
        test_dump_file = os.path.join(data_path, "test_dump_file.bin")
        with open(test_dump_file, "wb+") as f:
            f.write(b"\x00" * 1024 * 1024)
        with open(test_dump_file, "r+b") as f:
            f.seek(0)
            content = b"\x00\x00\x10\x00\x00\x00\x00\x00H\x00\x00\x00\x08\xfe\x0f\x00\xcd\xbc\xa5Z\x00\x00\x00\x00\xf8\x01\x80E\xc0\x12\x00\x00\x05\x00\x00\x00\x08\x00\x00\x00\x18\x00\x02\x01\x00\x00\x00\x00\x06\x00\x00\x00\x10\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xc5\xb8(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x005\xba(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00:\xba(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00;\xba(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x000\x00\x00\x00\x00\x00\x00\x00K\xba(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x00\x91\x00\x00\x00\x00\x00\x00\x00Y\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x00d\x00\x00\x00\x00\x00\x00\x00\xad\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x00c\x00\x00\x00\x00\x00\x00\x00\xbb\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x00e\x00\x00\x00\x00\x00\x00\x00\xbc\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\xdd\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\xe1\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\xe4\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\xf0\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\xf4\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\xf7\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\xfa\xbb(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\x02\xbc(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\x05\xbc(\x07\xd0\xe4\x00\x00\x06\x00\x00\x00\x10\x00\x00\x001\x00\x00\x00\x00\x00\x00\x00\x0b\xbc(\x07\xd0\xe4\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            f.write(content)
        glob_mock.return_value = [test_dump_file]
        dump_file = DumpBinFile(test_dump_file)
        self.assertEqual(os.path.basename(dump_file.dump_bin), "test_dump_file.bin")
        run_mock.assert_called_once()
        self._clean_out_dir(data_path)

    def test_data_converter_bf16(self):
        self.assertEqual(data_converter.decode_bfloat16(123), 1.1295766027432919e-38)
        self.assertEqual(data_converter.decode_bfloat16(123123), -1.400799628097319e20)
        self.assertEqual(data_converter.decode_bfloat16(0x3F80), 1)
        self.assertEqual(data_converter.decode_bfloat16(0xC000), -2)
        self.assertEqual(data_converter.decode_bfloat16(0x7F7F), 3.3895313892515355e38)
        self.assertEqual(data_converter.decode_bfloat16(0x0080), 1.1754943508222875e-38)
        self.assertEqual(data_converter.decode_bfloat16(0x0000), 0)
        self.assertEqual(data_converter.decode_bfloat16(0x8000), 0)
        self.assertTrue(math.isnan(data_converter.decode_bfloat16(0xFFC1)))
        self.assertTrue(math.isnan(data_converter.decode_bfloat16(0xFF81)))
        self.assertTrue(
            math.isinf(data_converter.decode_bfloat16(0x7F80))
            and data_converter.decode_bfloat16(0x7F80) > 0
        )
        self.assertTrue(
            math.isinf(data_converter.decode_bfloat16(0xFF80))
            and data_converter.decode_bfloat16(0xFF80) < 0
        )

    def test_get_existing_member_name(self):
        result = get_enum_member_name(TimeStampId, 48)
        self.assertEqual(result, "TIME_STAMP_TPIPE")

    def test_get_non_existing_member_value(self):
        result = get_enum_member_name(TimeStampId, 2457)
        self.assertEqual(result, 2457)

    def test_get_auto_enum_member(self):
        result = get_enum_member_name(TimeStampId, 1)
        self.assertEqual(result, "TIME_STAMP_WRAP_MC2_CTX")

    def test_write_to(self):
        tlv = TLV(tag=1, length=4, value=b"test")
        buffer = bytearray(tlv.get_size())
        offset = tlv.write_to(buffer, 0)
        self.assertEqual(offset, tlv.get_size())
        (tag, length) = struct.unpack("ii", buffer[:8])
        self.assertEqual(tag, 1)
        self.assertEqual(length, 4)

    def test_read(self):
        test_data = struct.pack("ii", 2, 5) + b"hello"
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(test_data)
            f.flush()
            f.seek(0)
            tlv = TLV()
            tlv.read(f)
            self.assertEqual(tlv.tag, 2)
            self.assertEqual(tlv.length, 5)
            self.assertEqual(tlv.value, b"hello")
        os.unlink(f.name)

    def test_unpack(self):
        header = DumpMessageHeader()
        buffer = struct.pack("iiiiii", 4096, 3, 1, 2, 0, 0)
        header.unpack(buffer)
        self.assertEqual(header.addr, 4096)
        self.assertEqual(header.data_type, 3)
        self.assertEqual(header.desc, 1)
        self.assertEqual(header.buffer_id, 2)

    def test_pack(self):
        header = DumpMessageHeader(
            addr=8192, data_type=0, desc=5, buffer_id=1, position=0, reserved=0
        )
        packed = header.pack()
        unpacked = struct.unpack("iiiiii", packed)
        self.assertEqual(unpacked[0], 8192)
        self.assertEqual(unpacked[1], 0)
        self.assertEqual(unpacked[2], 5)

    def test_unpack_shape_info(self):
        shape_info = ShapeInfo()
        buffer = struct.pack("iiiiiiiiii", 2, 3, 4, 0, 0, 0, 0, 0, 0, 0)
        shape_info.unpack(buffer)
        self.assertEqual(shape_info.dim, 2)
        self.assertEqual(shape_info.shape, [3, 4])
        self.assertEqual(shape_info.total_ele_num, 12)

    def test_parse_from(self):
        shape_info = ShapeInfo()
        buffer = struct.pack("iiiiiiiiii", 3, 2, 3, 4, 0, 0, 0, 0, 0, 0)
        tlv = TLV(tag=3, length=len(buffer), value=buffer)
        shape_info.parse_from(tlv)
        self.assertEqual(shape_info.dim, 3)
        self.assertEqual(shape_info.shape, [2, 3, 4])
        self.assertEqual(shape_info.total_ele_num, 24)

    def test_unpack_mixed_core(self):
        meta_info = MetaInfo()
        buffer = struct.pack("HbbI", 4, 0, 1, 0)
        meta_info.unpack(buffer)
        self.assertEqual(meta_info.blk_dim, 4)
        self.assertEqual(meta_info.core_type, 0)
        self.assertIn("MIX", meta_info.content)
        self.assertIn("block num: 4", meta_info.content)

    def test_unpack_aic_core(self):
        meta_info = MetaInfo()
        buffer = struct.pack("HbbI", 2, 1, 0, 0)
        meta_info.unpack(buffer)
        self.assertIn("AIC", meta_info.content)

    def test_unpack_vec_core(self):
        meta_info = MetaInfo()
        buffer = struct.pack("HbbI", 8, 2, 1, 0)
        meta_info.unpack(buffer)
        self.assertIn("VEC", meta_info.content)

    def test_parse_from_meta_info(self):
        meta_info = MetaInfo()
        buffer = struct.pack("HbbI", 4, 0, 1, 0)
        tlv = TLV(tag=5, length=len(buffer), value=buffer)
        meta_info.parse_from(tlv)
        self.assertEqual(meta_info.blk_dim, 4)

    def test_unpack_time_stamp_info(self):
        ts_info = TimeStampInfo()
        buffer = struct.pack("IIQQ", 49, 0, 1000, 2748)
        ts_info.unpack(buffer)
        self.assertEqual(ts_info.desc_id, 49)
        self.assertEqual(ts_info.sys_cycle, 1000)
        self.assertEqual(ts_info.pc_ptr, 2748)

    def test_parse_from_time_stamp_info(self):
        ts_info = TimeStampInfo()
        buffer = struct.pack("IIQQ", 48, 0, 2000, 256)
        tlv = TLV(tag=6, length=len(buffer), value=buffer)
        ts_info.parse_from(tlv)
        self.assertEqual(ts_info.desc_id, 48)
        self.assertEqual(ts_info.sys_cycle, 2000)

    def test_unpack_fifo_time_stamp_info(self):
        ts_info = FifoTimeStampInfo()
        buffer = struct.pack("IHHQQQII", 49, 3, 0, 1000, 2748, 1, 0, 0)
        ts_info.unpack(buffer)
        self.assertEqual(ts_info.desc_id, 49)
        self.assertEqual(ts_info.block_idx, 3)
        self.assertEqual(ts_info.sys_cycle, 1000)
        self.assertEqual(ts_info.entry, 1)

    def _merged_DumpTensor__create_tensor_tlv(
        self, addr=4096, data_type=3, desc=1, values=(7, 9)
    ):
        header = struct.pack("iiiiii", addr, data_type, desc, 0, 0, 0)
        tensor_bytes = b"".join((struct.pack("i", v) for v in values))
        tlv_value = header + tensor_bytes
        return TLV(
            tag=DumpType.TENSOR_TYPE.value, length=len(tlv_value), value=tlv_value
        )

    def test_parse_from_dump_tensor(self):
        tensor = DumpTensor()
        tlv = self._merged_DumpTensor__create_tensor_tlv(
            data_type=3, desc=5, values=(10, 20)
        )
        tensor.parse_from(tlv)
        self.assertEqual(tensor.tag, DumpType.TENSOR_TYPE.value)
        self.assertEqual(tensor.dump_header.data_type, 3)
        self.assertEqual(tensor.dump_header.desc, 5)
        self.assertEqual(tensor.dump_value, [10, 20])

    def test_parse_from_float(self):
        tensor = DumpTensor()
        header = struct.pack("iiiiii", 0, 0, 1, 0, 0, 0)
        tensor_bytes = struct.pack("ff", 1.5, 2.5)
        tlv = TLV(
            tag=DumpType.TENSOR_TYPE.value,
            length=len(header + tensor_bytes),
            value=header + tensor_bytes,
        )
        tensor.parse_from(tlv)
        self.assertEqual(len(tensor.dump_value), 2)
        self.assertAlmostEqual(tensor.dump_value[0], 1.5, places=5)
        self.assertAlmostEqual(tensor.dump_value[1], 2.5, places=5)

    def test_repr_dump_tensor(self):
        tensor = DumpTensor(tag=2, length=100)
        repr_str = repr(tensor)
        self.assertIn("tag=2", repr_str)
        self.assertIn("length=100", repr_str)

    def test_parse_to(self):
        tensor = DumpTensor()
        tlv = self._merged_DumpTensor__create_tensor_tlv()
        tensor.parse_from(tlv)
        result_tlv = tensor.parse_to()
        self.assertEqual(result_tlv.tag, tensor.tag)
        self.assertEqual(result_tlv.length, tensor.length)

    def _merged_FifoDumpTensor__create_fifo_tensor_tlv(
        self, desc=1, data_type=3, values=(7, 9)
    ):
        tensor_bytes = b"".join((struct.pack("i", v) for v in values))
        header = struct.pack(
            "IIIIHHI8III",
            4096,
            data_type,
            desc,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            len(tensor_bytes),
        )
        tlv_value = header + tensor_bytes
        return TLV(
            tag=DumpType.TENSOR_TYPE.value, length=len(tlv_value), value=tlv_value
        )

    def test_parse_from_fifo_dump_tensor(self):
        tensor = FifoDumpTensor()
        tlv = self._merged_FifoDumpTensor__create_fifo_tensor_tlv(
            desc=11, data_type=3, values=(100, 200)
        )
        tensor.parse_from(tlv)
        self.assertEqual(tensor.dump_header.desc, 11)
        self.assertEqual(tensor.dump_header.data_type, 3)
        self.assertEqual(tensor.dump_value, [100, 200])

    def test_parse_from_invalid_length(self):
        tensor = FifoDumpTensor()
        tlv = TLV(tag=DumpType.TENSOR_TYPE.value, length=10, value=b"\x00" * 10)
        with self.assertRaises(RuntimeError):
            tensor.parse_from(tlv)

    def test_parse_from_fifo_dump_tensor_dump_size_out_of_range(self):
        tensor = FifoDumpTensor()
        tensor_bytes = struct.pack("i", 100)
        # dump_size is intentionally larger than actual payload length.
        header = struct.pack(
            "IIIIHHI8III",
            4096,
            3,
            11,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            len(tensor_bytes) + 8,
        )
        tlv = TLV(
            tag=DumpType.TENSOR_TYPE.value,
            length=len(header + tensor_bytes),
            value=header + tensor_bytes,
        )
        with self.assertRaisesRegex(
            RuntimeError, "FifoDumpTensor: dump_size out of range"
        ):
            tensor.parse_from(tlv)

    def _merged_PrintStruct__create_print_tlv(self, fmt, args):
        fmt_bytes = fmt.encode("utf-8") + b"\x00"
        fmt_offset = 8
        args_bytes = b"".join(
            (
                struct.pack("q", arg)
                if isinstance(arg, int)
                else struct.pack("d", arg)
                if isinstance(arg, float)
                else b""
                for arg in args
            )
        )
        tlv_value = struct.pack("Q", fmt_offset) + args_bytes + fmt_bytes
        return TLV(
            tag=DumpType.SCALAR_TYPE.value, length=len(tlv_value), value=tlv_value
        )

    def test_read_arg_long(self):
        buffer = struct.pack("q", -12345)
        result = PrintStruct._read_arg_long(buffer, 0)
        self.assertEqual(result, -12345)

    def test_read_arg_unsigned_long(self):
        buffer = struct.pack("Q", 123456789)
        result = PrintStruct._read_arg_unsigned_long(buffer, 0)
        self.assertEqual(result, 123456789)

    def test_read_arg_double(self):
        buffer = struct.pack("d", 3.14159)
        result = PrintStruct._read_arg_double(buffer, 0)
        self.assertAlmostEqual(result, 3.14159, places=5)

    def test_read_arg_hex(self):
        buffer = struct.pack("Q", 3735928559)
        result = PrintStruct._read_arg_hex(buffer, 0)
        self.assertEqual(result, 3735928559)

    def test_read_string(self):
        buffer = b"hello\x00world\x00"
        result = PrintStruct._read_string(buffer, 0)
        self.assertEqual(result, "hello")

    def test_read_string_offset_over_max(self):
        buffer = b"hello"
        with self.assertRaises(RuntimeError):
            PrintStruct._read_string(buffer, 100)

    def test_all_fmt_placehold(self):
        ps = PrintStruct()
        ps.fmt = "value=%d, hex=%x, str=%s"
        result = ps._all_fmt_placehold()
        self.assertIn("%d", result)
        self.assertIn("%x", result)
        self.assertIn("%s", result)

    def _merged_FifoPrintStruct__create_fifo_print_tlv(
        self, fmt, value, block_idx=0, tag=None
    ):
        if tag is None:
            tag = DumpType.SCALAR_TYPE.value
        fmt_bytes = fmt.encode("utf-8") + b"\x00"
        fifo_payload = (
            struct.pack("IIQ", block_idx, 0, 16) + struct.pack("q", value) + fmt_bytes
        )
        return TLV(tag=tag, length=len(fifo_payload), value=fifo_payload)

    def _merged_FifoPrintStruct__create_fifo_simt_print_tlv(self, fmt, value, tag=None):
        if tag is None:
            tag = DumpType.SIMT_PRINTF_TYPE.value
        fmt_bytes = fmt.encode("utf-8") + b"\x00"
        simt_header = struct.pack("3I3I4IQ", 1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 16)
        simt_payload = simt_header + struct.pack("q", value) + fmt_bytes
        return TLV(tag=tag, length=len(simt_payload), value=simt_payload)

    def test_parse_from_fifo_print_struct(self):
        ps = FifoPrintStruct()
        tlv = self._merged_FifoPrintStruct__create_fifo_print_tlv(
            "value=%d\n", 42, block_idx=3
        )
        ps.parse_from(tlv)
        self.assertEqual(ps.block_idx, 3)
        self.assertEqual(ps.content, "value=42\n")

    def test_parse_from_fifo_simt_print_struct(self):
        ps = FifoSimtPrintStruct()
        tlv = self._merged_FifoPrintStruct__create_fifo_simt_print_tlv("simt=%d\n", 99)
        ps.parse_from(tlv)
        self.assertEqual(ps.block_idx, [1, 2, 3])
        self.assertEqual(ps.thread_idx, [4, 5, 6])
        self.assertEqual(ps.content, "simt=99\n")

    def test_parse_from_invalid_length_fifo_print_struct(self):
        ps = FifoPrintStruct()
        tlv = TLV(tag=DumpType.SCALAR_TYPE.value, length=8, value=b"\x00" * 8)
        with self.assertRaises(RuntimeError):
            ps.parse_from(tlv)

    def test_unpack_block_info(self):
        block_info = BlockInfo()
        buffer = struct.pack("iiiiiiQ", 1024 * 1024, 0, 1, 100, 1520811213, 0, 32768)
        block_info.unpack(buffer)
        self.assertEqual(block_info.total_size, 1024 * 1024)
        self.assertEqual(block_info.block_id, 0)
        self.assertEqual(block_info.magic_num, 1520811213)

    def test_pack_into(self):
        block_info = BlockInfo(
            total_size=1024,
            block_id=0,
            block_num=1,
            remain_size=0,
            magic_num=1520811213,
            reserved=0,
            dump_addr=4096,
        )
        buffer = bytearray(BlockInfo.get_size())
        offset = block_info.pack_into(buffer, 0)
        self.assertEqual(offset, BlockInfo.get_size())

    def test_is_valid(self):
        valid_block = BlockInfo(magic_num=1520811213)
        self.assertTrue(valid_block.is_valid())
        invalid_block = BlockInfo(magic_num=305419896)
        self.assertFalse(invalid_block.is_valid())

    def test_unpack_fifo_block_info(self):
        block_info = FifoBlockInfo()
        buffer = struct.pack(
            "IIIIHHIQ6I", 256, 5, 1, 0, 44678, 0, 0, 4096, 0, 0, 0, 0, 0, 0
        )
        block_info.unpack(buffer)
        self.assertEqual(block_info.length, 256)
        self.assertEqual(block_info.core_id, 5)
        self.assertEqual(block_info.magic, 44678)
        self.assertEqual(block_info.dump_addr, 4096)

    def test_is_valid_fifo_block_info(self):
        valid_block = FifoBlockInfo(magic=44678)
        self.assertTrue(valid_block.is_valid())
        invalid_block = FifoBlockInfo(magic=4660)
        self.assertFalse(invalid_block.is_valid())

    def test_repr_fifo_block_info(self):
        block_info = FifoBlockInfo(length=256, core_id=5, magic=44678)
        repr_str = repr(block_info)
        self.assertIn("length=256", repr_str)
        self.assertIn("core_id=5", repr_str)
        self.assertIn("magic=0xAE86", repr_str)

    def _merged_DumpCoreContent__create_tlv(self, tag, value):
        return TLV(tag=tag, length=len(value), value=value)

    def _merged_DumpCoreContent__create_tensor_value(
        self, desc=1, data_type=3, values=(1, 2)
    ):
        header = struct.pack("iiiiii", 0, data_type, desc, 0, 0, 0)
        tensor_bytes = b"".join((struct.pack("i", v) for v in values))
        return header + tensor_bytes

    def test_add_tlv_data_tensor(self):
        content = DumpCoreContent()
        tensor_value = self._merged_DumpCoreContent__create_tensor_value(
            desc=5, values=(10, 20)
        )
        tlv = self._merged_DumpCoreContent__create_tlv(
            DumpType.TENSOR_TYPE.value, tensor_value
        )
        content.add_tlv_data(tlv)
        self.assertIn(5, content.dump_tensor_map)
        self.assertEqual(content.dump_tensor_map[5][0].dump_value, [10, 20])

    def test_add_tlv_data_shape(self):
        content = DumpCoreContent()
        shape_value = struct.pack("iiiiiiiiii", 2, 3, 4, 0, 0, 0, 0, 0, 0, 0)
        tlv = self._merged_DumpCoreContent__create_tlv(
            DumpType.SHAPE_TYPE.value, shape_value
        )
        content.add_tlv_data(tlv)
        self.assertEqual(content.shape, [3, 4])

    def test_add_tlv_data_timestamp(self):
        content = DumpCoreContent()
        ts_value = struct.pack("IIQQ", 49, 0, 1000, 2748)
        tlv = self._merged_DumpCoreContent__create_tlv(
            DumpType.TIME_STAMP.value, ts_value
        )
        content.add_tlv_data(tlv)
        self.assertEqual(len(content.time_stamp_list), 1)
        self.assertEqual(content.time_stamp_list[0].desc_id, 49)

    def test_add_tlv_data_meta(self):
        content = DumpCoreContent()
        meta_value = struct.pack("HbbI", 4, 0, 1, 0)
        tlv = self._merged_DumpCoreContent__create_tlv(
            DumpType.META_TYPE.value, meta_value
        )
        content.add_tlv_data(tlv)
        self.assertEqual(len(content.print_list), 1)
        self.assertIn("block num: 4", content.print_list[0])

    def test_write_dump_tensor_value_shape_mismatch_fill_dash(self):
        content = DumpCoreContent()
        tensor = DumpTensor()
        tensor.dump_header = DumpMessageHeader(data_type=3, desc=1)
        tensor.dump_data = struct.pack("ii", 1, 2)
        tensor.dump_value = [1, 2]
        tensor.dump_shape = [3, 3]
        with tempfile.NamedTemporaryFile("w+", delete=False) as f:
            output_file = f.name
        try:
            content._write_dump_tensor_value(tensor, output_file)
            with open(output_file, "r") as f:
                result = f.read()
            self.assertIn("-", result)
            self.assertEqual(result.count("-"), 7)
        finally:
            if os.path.exists(output_file):
                os.unlink(output_file)

    def test_write_dump_tensor_value_shape_smaller_truncate_extra(self):
        content = DumpCoreContent()
        tensor = DumpTensor()
        tensor.dump_header = DumpMessageHeader(data_type=3, desc=1)
        tensor.dump_data = struct.pack("iiii", 11, 22, 33, 44)
        tensor.dump_value = [11, 22, 33, 44]
        tensor.dump_shape = [1, 2]
        with tempfile.NamedTemporaryFile("w+", delete=False) as f:
            output_file = f.name
        try:
            content._write_dump_tensor_value(tensor, output_file)
            with open(output_file, "r") as f:
                result = f.read()
            self.assertIn("11", result)
            self.assertIn("22", result)
            self.assertNotIn("33", result)
            self.assertNotIn("44", result)
        finally:
            if os.path.exists(output_file):
                os.unlink(output_file)

    def _merged_FifoDumpCoreContent__build_fifo_block_info(self, core_id=0, length=64):
        return struct.pack(
            FifoBlockInfo.get_format(),
            length,
            core_id,
            1,
            0,
            44678,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        )

    def _merged_FifoDumpCoreContent__build_tlv(self, tag, value):
        return struct.pack(TLV.get_tl_format(), tag, len(value)) + value

    def _merged_FifoDumpCoreContent__build_fifo_tensor_tlv(
        self, desc=1, data_type=3, values=(7, 9)
    ):
        tensor_bytes = b"".join((struct.pack("i", v) for v in values))
        header = struct.pack(
            "IIIIHHI8III",
            0,
            data_type,
            desc,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            len(tensor_bytes),
        )
        return self._merged_FifoDumpCoreContent__build_tlv(
            DumpType.TENSOR_TYPE.value, header + tensor_bytes
        )

    def test_add_tlv_data_fifo_tensor(self):
        content = FifoDumpCoreContent()
        tlv_data = self._merged_FifoDumpCoreContent__build_fifo_tensor_tlv(
            desc=11, values=(100, 200)
        )
        tlv = TLV()
        (tlv.tag, tlv.length) = struct.unpack(
            TLV.get_tl_format(), tlv_data[: TLV.get_tl_size()]
        )
        tlv.value = tlv_data[TLV.get_tl_size() :]
        content.add_tlv_data(tlv)
        self.assertIn(11, content.dump_tensor_map)
        self.assertEqual(content.dump_tensor_map[11][0].dump_value, [100, 200])

    def test_add_tlv_data_fifo_simt_print_and_assert(self):
        content = FifoDumpCoreContent()
        simt_print_tlv = self._merged_FifoPrintStruct__create_fifo_simt_print_tlv(
            "simt_print=%d\n", 7
        )
        simt_assert_tlv = self._merged_FifoPrintStruct__create_fifo_simt_print_tlv(
            "simt_assert=%d\n", 1, tag=DumpType.SIMT_ASSERT_TYPE.value
        )
        content.add_tlv_data(simt_print_tlv)
        content.add_tlv_data(simt_assert_tlv)
        self.assertEqual(len(content.print_list), 2)
        self.assertEqual(content.print_list[0], "simt_print=7\n")
        self.assertEqual(content.print_list[1], "simt_assert=1\n")
        self.assertIn("4_5_6", content.simt_print_map)
        self.assertEqual(
            content.simt_print_map["4_5_6"], ["simt_print=7\n", "simt_assert=1\n"]
        )

    def test_get_install_path_success(self):
        with patch.dict(os.environ, {"ASCEND_HOME_PATH": "/usr/local/ascend"}):
            result = dump_parser.get_install_path()
            self.assertEqual(result, "/usr/local/ascend")

    def test_get_install_path_failure(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(RuntimeError) as context:
                dump_parser.get_install_path()
            self.assertIn("install path env failed", str(context.exception))

    def test_help_flag(self):
        with patch("sys.argv", ["show_kernel_debug_data", "-h"]):
            with patch("sys.exit"):
                dump_parser.execute_parse()

    def test_invalid_params(self):
        with patch("sys.argv", ["show_kernel_debug_data"]):
            with self.assertRaises(RuntimeError):
                dump_parser.execute_parse()

    def test_file_not_exist(self):
        with patch(
            "sys.argv", ["show_kernel_debug_data", "/nonexistent/file.bin", "/tmp"]
        ):
            with self.assertRaises(RuntimeError) as context:
                dump_parser.execute_parse()
            self.assertIn("does not exist", str(context.exception))

    def test_directory_discovery_keeps_literal_glob_characters(self):
        for dirname, sibling in (
            ("input[1]", "input1"),
            ("input*", "input-other"),
            ("input?", "input2"),
        ):
            with self.subTest(dirname=dirname):
                input_dir = os.path.join(self.test_dir, dirname)
                nested = os.path.join(input_dir, "nested")
                neighbor = os.path.join(self.test_dir, sibling)
                os.makedirs(nested)
                os.makedirs(neighbor)
                expected = os.path.join(nested, "actual.bin")
                decoy = os.path.join(neighbor, "other.bin")
                for filename in (expected, decoy):
                    with open(filename, "wb") as stream:
                        stream.write(b"\x00")

                self.assertEqual(dump_parser._collect_bin_files(input_dir), [expected])
                with (
                    patch(
                        "sys.argv", ["show_kernel_debug_data", input_dir, self.test_dir]
                    ),
                    patch(
                        "show_kernel_debug_data.dump_parser.parse_dump_bin",
                        return_value=0,
                    ) as parse_mock,
                ):
                    self.assertEqual(dump_parser.execute_parse(), 0)
                    self.assertEqual(parse_mock.call_count, 1)
                    self.assertEqual(parse_mock.call_args.args[0], expected)

    def test_directory_mode(self):
        input_dir = os.path.join(self.test_dir, "input_bins")
        sub_dir = os.path.join(input_dir, "sub")
        os.makedirs(sub_dir)
        file_a = os.path.join(input_dir, "a.bin")
        file_b = os.path.join(sub_dir, "b.bin")
        with open(file_a, "wb") as f:
            f.write(b"\x00")
        with open(file_b, "wb") as f:
            f.write(b"\x00")
        with open(os.path.join(input_dir, "ignore.txt"), "w") as f:
            f.write("x")

        with (
            patch("sys.argv", ["show_kernel_debug_data", input_dir, self.test_dir]),
            patch(
                "show_kernel_debug_data.dump_parser.parse_dump_bin", return_value=0
            ) as parse_dump_bin_mock,
        ):
            dump_parser.execute_parse()
            expected_calls = [
                call(
                    os.path.abspath(file_a),
                    self.test_dir,
                    parse_output_dir=ANY,
                    init_logger=False,
                ),
                call(
                    os.path.abspath(file_b),
                    self.test_dir,
                    parse_output_dir=ANY,
                    init_logger=False,
                ),
            ]
            parse_dump_bin_mock.assert_has_calls(expected_calls, any_order=False)
            self.assertEqual(parse_dump_bin_mock.call_count, 2)
            parser_dirs = {
                call_item.kwargs.get("parse_output_dir")
                for call_item in parse_dump_bin_mock.call_args_list
            }
            self.assertEqual(len(parser_dirs), 1)

    def test_directory_mode_without_bin(self):
        input_dir = os.path.join(self.test_dir, "empty_dir")
        os.makedirs(input_dir)
        with patch("sys.argv", ["show_kernel_debug_data", input_dir, self.test_dir]):
            with self.assertRaises(RuntimeError) as context:
                dump_parser.execute_parse()
            self.assertIn("does not contain any .bin file", str(context.exception))

    @patch("show_kernel_debug_data.parse_dump_bin")
    def test_api_auto_create_output_dir_for_file(self, parse_dump_bin_mock):
        test_file = os.path.join(self.test_dir, "api_input.bin")
        with open(test_file, "wb") as f:
            f.write(b"\x00")
        output_dir = os.path.join(self.test_dir, "api_out")
        show_kernel_debug_data_api(test_file, output_dir)
        self.assertTrue(os.path.isdir(output_dir))
        parse_dump_bin_mock.assert_called_once_with(
            os.path.abspath(test_file), output_dir
        )

    @patch("show_kernel_debug_data.DUMP_PARSER_LOG.set_log_level")
    @patch("show_kernel_debug_data.DUMP_PARSER_LOG.set_log_file")
    @patch("show_kernel_debug_data._make_parser_output_dir")
    @patch("show_kernel_debug_data.parse_dump_bin")
    def test_api_directory_mode(
        self,
        parse_dump_bin_mock,
        make_parser_output_dir_mock,
        set_log_file_mock,
        set_log_level_mock,
    ):
        input_dir = os.path.join(self.test_dir, "api_input_dir")
        sub_dir = os.path.join(input_dir, "sub")
        os.makedirs(sub_dir)
        file_a = os.path.join(input_dir, "a.bin")
        file_b = os.path.join(sub_dir, "b.bin")
        with open(file_a, "wb") as f:
            f.write(b"\x00")
        with open(file_b, "wb") as f:
            f.write(b"\x00")

        output_dir = os.path.join(self.test_dir, "api_output_dir")
        parser_output_dir = os.path.join(self.test_dir, "PARSER_FIXED")
        os.makedirs(parser_output_dir)
        make_parser_output_dir_mock.return_value = parser_output_dir

        show_kernel_debug_data_api(input_dir, output_dir)

        set_log_file_mock.assert_called_once_with(
            os.path.join(parser_output_dir, "parser.log")
        )
        set_log_level_mock.assert_called_once()
        expected_calls = [
            call(
                os.path.abspath(file_a),
                output_dir,
                parse_output_dir=parser_output_dir,
                init_logger=False,
            ),
            call(
                os.path.abspath(file_b),
                output_dir,
                parse_output_dir=parser_output_dir,
                init_logger=False,
            ),
        ]
        parse_dump_bin_mock.assert_has_calls(expected_calls, any_order=False)
        self.assertEqual(parse_dump_bin_mock.call_count, 2)

    def test_parse_empty_file(self):
        test_file = os.path.join(self.test_dir, "empty.bin")
        with open(test_file, "wb") as f:
            f.write(b"")
        dump_file = DumpBinFile(test_file)
        dump_file.parse()
        self.assertEqual(len(dump_file.dump_core_contents), 0)

    def test_write_result_empty(self):
        test_file = os.path.join(self.test_dir, "empty.bin")
        with open(test_file, "wb") as f:
            f.write(b"")
        dump_file = DumpBinFile(test_file)
        result = dump_file.write_result(self.test_dir)
        self.assertEqual(result, "")

    def test_show_print_empty(self):
        test_file = os.path.join(self.test_dir, "empty.bin")
        with open(test_file, "wb") as f:
            f.write(b"")
        dump_file = DumpBinFile(test_file)
        dump_file.show_print()

    def _merged_FifoDumpBinFile__build_fifo_block_info(self, core_id=0, length=64):
        return struct.pack(
            FifoBlockInfo.get_format(),
            length,
            core_id,
            1,
            0,
            44678,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        )

    def test_parse_minimal(self):
        content = self._merged_FifoDumpBinFile__build_fifo_block_info(
            core_id=5, length=FifoBlockInfo.get_size()
        )
        test_file = os.path.join(self.test_dir, "fifo_minimal.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "5")
        fifo_dump.parse()
        self.assertEqual(len(fifo_dump.dump_core_contents), 1)
        self.assertEqual(fifo_dump.dump_core_contents[0].block_info.core_id, 5)

    @staticmethod
    def _build_fifo_tlv(tag, value):
        return TLV(tag=tag, length=len(value), value=value)

    @staticmethod
    def _serialize_tlv(tlv):
        return struct.pack(TLV.get_tl_format(), tlv.tag, tlv.length) + tlv.value

    def _build_super_tensor_head(self, data, tensor_length, desc=77, tensor_offset=0):
        aligned_data = data + b"\x00" * ((-len(data)) % 32)
        value = struct.pack(
            "<IIIIHHI8II4xQQI4x",
            0,
            3,
            desc,
            0,
            0,
            0,
            0,
            *([0] * 8),
            0,
            tensor_length,
            tensor_offset,
            len(data),
        )
        return self._build_fifo_tlv(11, value + aligned_data)

    def _build_super_tensor_body(self, data, tensor_length, tensor_offset):
        aligned_data = data + b"\x00" * ((-len(data)) % 32)
        value = struct.pack("<IIQQI4x", 0, 0, tensor_length, tensor_offset, len(data))
        return self._build_fifo_tlv(12, value + aligned_data)

    def test_parse_super_tensor_assembles_one_logical_tensor(self):
        tensor_data = struct.pack("10i", *range(10))
        content = FifoDumpCoreContent()
        content.block_info = FifoBlockInfo(core_id=5, magic=44678)
        content.add_tlv_data(
            self._build_super_tensor_head(tensor_data[:16], len(tensor_data))
        )
        content.add_tlv_data(
            self._build_super_tensor_body(tensor_data[16:32], len(tensor_data), 16)
        )
        content.add_tlv_data(
            self._build_super_tensor_body(tensor_data[32:], len(tensor_data), 32)
        )

        tensors = content.dump_tensor_map[77]
        self.assertEqual(len(tensors), 1)
        self.assertEqual(tensors[0].dump_data, tensor_data)
        self.assertEqual(tensors[0].dump_value, list(range(10)))
        content.write_to_dir(self.test_dir)
        dump_file = os.path.join(
            self.test_dir, "5", "index_77", "core_5_index_77_loop_0.bin"
        )
        with open(dump_file, "rb") as f:
            self.assertEqual(f.read(), tensor_data)

    def test_super_tensor_body_without_head_is_rejected(self):
        content = FifoDumpCoreContent()
        with self.assertRaisesRegex(RuntimeError, "body without head"):
            content.add_tlv_data(self._build_super_tensor_body(b"1234", 8, 4))

    def test_super_tensor_head_with_nonzero_offset_is_rejected(self):
        content = FifoDumpCoreContent()
        with self.assertRaisesRegex(RuntimeError, "invalid super tensor offset"):
            content.add_tlv_data(
                self._build_super_tensor_head(b"1234", 8, tensor_offset=4)
            )

    def test_super_tensor_duplicate_head_is_ignored(self):
        content = FifoDumpCoreContent()
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 8))
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 8))
        content.add_tlv_data(self._build_super_tensor_body(b"5678", 8, 4))
        self.assertEqual(content.dump_tensor_map[77][0].dump_data, b"12345678")

    def test_super_tensor_conflicting_duplicate_head_is_rejected(self):
        content = FifoDumpCoreContent()
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 8))
        with self.assertRaisesRegex(RuntimeError, "new super tensor"):
            content.add_tlv_data(self._build_super_tensor_head(b"5678", 8))
        self.assertEqual(content.dump_tensor_map, {})

    def test_super_tensor_duplicate_body_is_ignored(self):
        content = FifoDumpCoreContent()
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 12))
        content.add_tlv_data(self._build_super_tensor_body(b"5678", 12, 4))
        content.add_tlv_data(self._build_super_tensor_body(b"5678", 12, 4))
        content.add_tlv_data(self._build_super_tensor_body(b"90ab", 12, 8))
        self.assertEqual(content.dump_tensor_map[77][0].dump_data, b"1234567890ab")

    def test_super_tensor_replayed_tail_after_completion_is_ignored(self):
        content = FifoDumpCoreContent()
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 8))
        tail = self._build_super_tensor_body(b"5678", 8, 4)
        content.add_tlv_data(tail)
        content.add_tlv_data(tail)
        self.assertEqual(len(content.dump_tensor_map[77]), 1)
        self.assertEqual(content.dump_tensor_map[77][0].dump_data, b"12345678")

    def test_super_tensor_non_contiguous_offset_is_rejected(self):
        content = FifoDumpCoreContent()
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 12))
        with self.assertRaisesRegex(RuntimeError, "non-contiguous"):
            content.add_tlv_data(self._build_super_tensor_body(b"5678", 12, 8))
        self.assertEqual(content.dump_tensor_map, {})

    def test_super_tensor_length_change_is_rejected(self):
        content = FifoDumpCoreContent()
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 12))
        with self.assertRaisesRegex(RuntimeError, "length changed"):
            content.add_tlv_data(self._build_super_tensor_body(b"5678", 16, 4))
        self.assertEqual(content.dump_tensor_map, {})

    def test_super_tensor_overrun_is_rejected(self):
        content = FifoDumpCoreContent()
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 6))
        with self.assertRaisesRegex(RuntimeError, "exceeds tensor length"):
            content.add_tlv_data(self._build_super_tensor_body(b"5678", 6, 4))
        self.assertEqual(content.dump_tensor_map, {})

    def test_super_tensor_incomplete_at_eof_is_rejected(self):
        content = FifoDumpCoreContent()
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 8))
        with self.assertRaisesRegex(RuntimeError, "incomplete super tensor"):
            content.ensure_super_tensor_complete()
        self.assertEqual(content.dump_tensor_map, {})

    def test_super_tensor_interleaved_tlv_is_rejected(self):
        content = FifoDumpCoreContent()
        content.add_tlv_data(self._build_super_tensor_head(b"1234", 8))
        with self.assertRaisesRegex(RuntimeError, "unexpected TLV"):
            content.add_tlv_data(
                TLV(tag=DumpType.TIME_STAMP.value, length=0, value=b"")
            )
        self.assertEqual(content.dump_tensor_map, {})

    def test_fifo_parser_ignores_unused_tail_after_complete_super_tensor(self):
        tensor_data = struct.pack("i", 7)
        block_info = struct.pack(
            FifoBlockInfo.get_format(),
            32768,
            5,
            1,
            64,
            44678,
            0,
            0,
            0,
            *([0] * 6),
        )
        content = block_info + self._serialize_tlv(
            self._build_super_tensor_head(tensor_data, len(tensor_data))
        )
        content += struct.pack("f", 1.2) * 8
        test_file = os.path.join(self.test_dir, "fifo_unused_tail.bin")
        with open(test_file, "wb") as f:
            f.write(content)

        fifo_dump = FifoDumpBinFile(test_file, "aiv", "5")
        fifo_dump.parse()

        self.assertEqual(
            fifo_dump.dump_core_contents[0].dump_tensor_map[77][0].dump_data,
            tensor_data,
        )

    def test_fifo_parser_rejects_unused_tail_during_incomplete_super_tensor(self):
        block_info = struct.pack(
            FifoBlockInfo.get_format(),
            32768,
            5,
            1,
            64,
            44678,
            0,
            0,
            0,
            *([0] * 6),
        )
        content = block_info + self._serialize_tlv(
            self._build_super_tensor_head(b"1234", 8)
        )
        content += struct.pack("f", 1.2) * 8
        test_file = os.path.join(self.test_dir, "fifo_incomplete_tail.bin")
        with open(test_file, "wb") as f:
            f.write(content)

        fifo_dump = FifoDumpBinFile(test_file, "aiv", "5")
        with self.assertRaisesRegex(RuntimeError, "TLV length overflow"):
            fifo_dump.parse()

    def test_fifo_parser_skips_replayed_super_tensor_head_tail_before_skip(self):
        tensor_data = struct.pack("3i", 7, 8, 9)
        block_info = struct.pack(
            FifoBlockInfo.get_format(),
            32768,
            5,
            1,
            208,
            44678,
            0,
            0,
            0,
            *([0] * 6),
        )
        content = block_info
        content += self._serialize_tlv(
            self._build_super_tensor_head(tensor_data[:4], len(tensor_data))
        )
        content += self._serialize_tlv(
            self._build_super_tensor_body(tensor_data[4:8], len(tensor_data), 4)
        )
        content += struct.pack("f", 1.2) * 14
        content += struct.pack(TLV.get_tl_format(), 10, 0)
        content += self._serialize_tlv(
            self._build_super_tensor_body(tensor_data[8:], len(tensor_data), 8)
        )
        test_file = os.path.join(self.test_dir, "fifo_replayed_head_tail.bin")
        with open(test_file, "wb") as f:
            f.write(content)

        fifo_dump = FifoDumpBinFile(test_file, "aic", "5")
        fifo_dump.parse()

        self.assertEqual(
            fifo_dump.dump_core_contents[0].dump_tensor_map[77][0].dump_data,
            tensor_data,
        )

    def test_fifo_parser_rejects_replayed_tail_candidate_off_ring_boundary(self):
        block_info = struct.pack(
            FifoBlockInfo.get_format(),
            32768,
            5,
            1,
            216,
            44678,
            0,
            0,
            0,
            *([0] * 6),
        )
        content = block_info
        content += self._serialize_tlv(self._build_super_tensor_head(b"1234", 12))
        content += self._serialize_tlv(self._build_super_tensor_body(b"5678", 12, 4))
        content += struct.pack("f", 1.2) * 14
        content += struct.pack(TLV.get_tl_format(), 10, 0)
        test_file = os.path.join(self.test_dir, "fifo_false_replayed_head_tail.bin")
        with open(test_file, "wb") as f:
            f.write(content)

        fifo_dump = FifoDumpBinFile(test_file, "aic", "5")
        with self.assertRaisesRegex(RuntimeError, "TLV length overflow"):
            fifo_dump.parse()

    def test_parse_invalid_magic(self):
        content = struct.pack(
            FifoBlockInfo.get_format(), 64, 5, 1, 0, 4660, 0, 0, 0, 0, 0, 0, 0, 0, 0
        )
        test_file = os.path.join(self.test_dir, "invalid_magic.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "5")
        with self.assertRaises(RuntimeError):
            fifo_dump.parse()

    def _merged_PrintStructFormats__create_print_tlv_with_args(self, fmt, args_bytes):
        fmt_bytes = fmt.encode("utf-8") + b"\x00"
        fmt_offset = 8 + len(args_bytes)
        tlv_value = struct.pack("Q", fmt_offset) + args_bytes + fmt_bytes
        return TLV(
            tag=DumpType.SCALAR_TYPE.value, length=len(tlv_value), value=tlv_value
        )

    def test_format_u(self):
        ps = PrintStruct()
        args_bytes = struct.pack("Q", 4294967295)
        tlv = self._merged_PrintStructFormats__create_print_tlv_with_args(
            "value=%u", args_bytes
        )
        ps.parse_from(tlv)
        self.assertEqual(ps.content, "value=4294967295")

    def test_format_ld(self):
        ps = PrintStruct()
        args_bytes = struct.pack("q", -9876543210)
        tlv = self._merged_PrintStructFormats__create_print_tlv_with_args(
            "long=%ld", args_bytes
        )
        ps.parse_from(tlv)
        self.assertEqual(ps.content, "long=-9876543210")

    def test_format_x(self):
        ps = PrintStruct()
        args_bytes = struct.pack("Q", 3735928559)
        tlv = self._merged_PrintStructFormats__create_print_tlv_with_args(
            "hex=%x", args_bytes
        )
        ps.parse_from(tlv)
        self.assertIn("deadbeef", ps.content.lower())

    def test_escaped_percent_before_placeholder_text(self):
        ps = PrintStruct()
        args_bytes = struct.pack("Q", 42)
        tlv = self._merged_PrintStructFormats__create_print_tlv_with_args(
            "literal=%%d value=%u", args_bytes
        )
        ps.parse_from(tlv)
        self.assertEqual(ps.content, "literal=%d value=42")

    def test_escaped_percent_before_pointer_placeholder_text(self):
        ps = PrintStruct()
        args_bytes = struct.pack("P", 0x1234)
        tlv = self._merged_PrintStructFormats__create_print_tlv_with_args(
            "literal=%%p value=%p", args_bytes
        )
        ps.parse_from(tlv)
        self.assertEqual(ps.content, "literal=%p value=0x1234")

    def test_format_f(self):
        ps = PrintStruct()
        float_val = 3.14
        float_bytes = struct.pack("f", float_val) + b"\x00" * 4
        args_bytes = float_bytes
        tlv = self._merged_PrintStructFormats__create_print_tlv_with_args(
            "float=%f", args_bytes
        )
        ps.parse_from(tlv)
        self.assertIn("float=", ps.content)

    def test_format_lf(self):
        ps = PrintStruct()
        args_bytes = struct.pack("d", 3.14159265358979)
        tlv = self._merged_PrintStructFormats__create_print_tlv_with_args(
            "double=%lf", args_bytes
        )
        ps.parse_from(tlv)
        self.assertIn("double=", ps.content)

    def test_format_s(self):
        ps = PrintStruct()
        test_str = "hello"
        str_bytes = test_str.encode("utf-8") + b"\x00"
        rel_offset = 8
        args_bytes = struct.pack("Q", rel_offset) + str_bytes
        fmt_bytes = b"str=%s\x00"
        fmt_offset = 8 + len(args_bytes)
        tlv_value = struct.pack("Q", fmt_offset) + args_bytes + fmt_bytes
        tlv = TLV(
            tag=DumpType.SCALAR_TYPE.value, length=len(tlv_value), value=tlv_value
        )
        ps.parse_from(tlv)
        self.assertEqual(ps.content, "str=hello")

    def test_unsupported_format(self):
        ps = PrintStruct()
        args_bytes = struct.pack("q", 123)
        tlv = self._merged_PrintStructFormats__create_print_tlv_with_args(
            "bad=%c", args_bytes
        )
        with self.assertRaises(RuntimeError):
            ps.parse_from(tlv)

    def _merged_FifoDumpCoreContentWrite__build_fifo_block_info(
        self, core_id=0, length=64
    ):
        return struct.pack(
            FifoBlockInfo.get_format(),
            length,
            core_id,
            1,
            0,
            44678,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        )

    def _merged_FifoDumpCoreContentWrite__build_tlv(self, tag, value):
        return struct.pack(TLV.get_tl_format(), tag, len(value)) + value

    def _merged_FifoDumpCoreContentWrite__build_fifo_tensor_tlv(
        self, desc=1, data_type=3, values=(7, 9)
    ):
        tensor_bytes = b"".join((struct.pack("i", v) for v in values))
        header = struct.pack(
            "IIIIHHI8III",
            0,
            data_type,
            desc,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            len(tensor_bytes),
        )
        return self._merged_FifoDumpCoreContentWrite__build_tlv(
            DumpType.TENSOR_TYPE.value, header + tensor_bytes
        )

    def _merged_FifoDumpCoreContentWrite__build_fifo_shape_tlv(
        self, dim=2, shape=(3, 4)
    ):
        shape_list = list(shape) + [0] * (8 - len(shape))
        shape_value = struct.pack("i" * 10, dim, *shape_list, 0)
        return self._merged_FifoDumpCoreContentWrite__build_tlv(
            DumpType.SHAPE_TYPE.value, shape_value
        )

    def test_write_to_dir_with_tensors(self):
        content = FifoDumpCoreContent()
        block_info = FifoBlockInfo(core_id=7, magic=44678)
        content.block_info = block_info
        shape_tlv = self._merged_FifoDumpCoreContentWrite__build_fifo_shape_tlv(
            dim=2, shape=(2, 3)
        )
        tlv = TLV()
        (tlv.tag, tlv.length) = struct.unpack(
            TLV.get_tl_format(), shape_tlv[: TLV.get_tl_size()]
        )
        tlv.value = shape_tlv[TLV.get_tl_size() :]
        content.add_tlv_data(tlv)
        tensor_tlv = self._merged_FifoDumpCoreContentWrite__build_fifo_tensor_tlv(
            desc=1, values=(1, 2, 3, 4, 5, 6)
        )
        tlv2 = TLV()
        (tlv2.tag, tlv2.length) = struct.unpack(
            TLV.get_tl_format(), tensor_tlv[: TLV.get_tl_size()]
        )
        tlv2.value = tensor_tlv[TLV.get_tl_size() :]
        content.add_tlv_data(tlv2)
        content.write_to_dir(self.test_dir)
        core_dir = os.path.join(self.test_dir, "7")
        self.assertTrue(os.path.exists(core_dir))

    def test_write_dump_tensor_value_with_shape(self):
        content = FifoDumpCoreContent()
        block_info = FifoBlockInfo(core_id=1, magic=44678)
        content.block_info = block_info
        tensor = FifoDumpTensor()
        tensor.tag = DumpType.TENSOR_TYPE.value
        tensor.dump_header = DumpMessageHeader(data_type=3, desc=1)
        tensor.dump_data = struct.pack("iiii", 1, 2, 3, 4)
        tensor.dump_value = [1, 2, 3, 4]
        tensor.dump_shape = [2, 2]
        tensor.length = len(tensor.dump_data)
        content._add_dump_tensor(tensor)
        output_file = os.path.join(self.test_dir, "test_shape.txt")
        content._write_dump_tensor_value(tensor, output_file)
        self.assertTrue(os.path.exists(output_file))
        with open(output_file, "r") as f:
            result = f.read()
        self.assertIn("[", result)

    def test_write_dump_tensor_value_shape_mismatch(self):
        content = FifoDumpCoreContent()
        block_info = FifoBlockInfo(core_id=1, magic=44678)
        content.block_info = block_info
        tensor = FifoDumpTensor()
        tensor.dump_header = DumpMessageHeader(data_type=3, desc=1)
        tensor.dump_data = struct.pack("ii", 1, 2)
        tensor.dump_value = [1, 2]
        tensor.dump_shape = [3, 3]
        tensor.length = len(tensor.dump_data)
        output_file = os.path.join(self.test_dir, "test_mismatch.txt")
        content._write_dump_tensor_value(tensor, output_file)
        self.assertTrue(os.path.exists(output_file))
        with open(output_file, "r") as f:
            result = f.read()
        self.assertIn("-", result)
        self.assertEqual(result.count("-"), 7)

    def test_write_dump_tensor_value_shape_smaller_than_dump(self):
        content = FifoDumpCoreContent()
        block_info = FifoBlockInfo(core_id=1, magic=44678)
        content.block_info = block_info
        tensor = FifoDumpTensor()
        tensor.dump_header = DumpMessageHeader(data_type=3, desc=1)
        tensor.dump_data = struct.pack("iiii", 11, 22, 33, 44)
        tensor.dump_value = [11, 22, 33, 44]
        tensor.dump_shape = [1, 2]
        tensor.length = len(tensor.dump_data)
        output_file = os.path.join(self.test_dir, "test_shape_smaller.txt")
        content._write_dump_tensor_value(tensor, output_file)
        self.assertTrue(os.path.exists(output_file))
        with open(output_file, "r") as f:
            result = f.read()
        self.assertIn("11", result)
        self.assertIn("22", result)
        self.assertNotIn("33", result)
        self.assertNotIn("44", result)

    def test_write_time_stamp(self):
        content = FifoDumpCoreContent()
        block_info = FifoBlockInfo(core_id=2, magic=44678)
        content.block_info = block_info
        ts = FifoTimeStampInfo()
        ts.desc_id = 48
        ts.sys_cycle = 1000
        ts.pc_ptr = 2748
        content.time_stamp_list.append(ts)
        content._write_time_stamp(self.test_dir)
        csv_file = os.path.join(self.test_dir, "time_stamp_core_2.csv")
        self.assertTrue(os.path.exists(csv_file))

    def _merged_FifoDumpBinFileWrite__build_fifo_block_info(self, core_id=0, length=64):
        return struct.pack(
            FifoBlockInfo.get_format(),
            length,
            core_id,
            1,
            0,
            44678,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        )

    def _merged_FifoDumpBinFileWrite__build_tlv(self, tag, value):
        return struct.pack(TLV.get_tl_format(), tag, len(value)) + value

    def _merged_FifoDumpBinFileWrite__build_fifo_tensor_tlv(
        self, desc=1, data_type=3, values=(7, 9)
    ):
        tensor_bytes = b"".join((struct.pack("i", v) for v in values))
        header = struct.pack(
            "IIIIHHI8III",
            0,
            data_type,
            desc,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            len(tensor_bytes),
        )
        return self._merged_FifoDumpBinFileWrite__build_tlv(
            DumpType.TENSOR_TYPE.value, header + tensor_bytes
        )

    def _merged_FifoDumpBinFileWrite__build_fifo_timestamp_tlv(
        self, desc_id=48, block_idx=0, sys_cycle=1000
    ):
        ts_value = struct.pack(
            "IHHQQQII", desc_id, block_idx, 0, sys_cycle, 2748, 1, 0, 0
        )
        return self._merged_FifoDumpBinFileWrite__build_tlv(
            DumpType.TIME_STAMP.value, ts_value
        )

    def _merged_FifoDumpBinFileWrite__build_fifo_simt_print_tlv(
        self, fmt="simt=%d\n", value=11, tag=None
    ):
        if tag is None:
            tag = DumpType.SIMT_PRINTF_TYPE.value
        fmt_bytes = fmt.encode("utf-8") + b"\x00"
        simt_header = struct.pack("3I3I4IQ", 1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 16)
        simt_payload = simt_header + struct.pack("q", value) + fmt_bytes
        return self._merged_FifoDumpBinFileWrite__build_tlv(tag, simt_payload)

    def test_write_result_with_tensors(self):
        content = self._merged_FifoDumpBinFileWrite__build_fifo_block_info(
            core_id=3, length=256
        )
        content += self._merged_FifoDumpBinFileWrite__build_fifo_tensor_tlv(
            desc=5, values=(10, 20, 30)
        )
        test_file = os.path.join(self.test_dir, "write_test.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "3")
        fifo_dump.parse()
        result_dir = fifo_dump.write_result(self.test_dir)
        self.assertTrue(os.path.exists(result_dir))

    def test_write_result_with_timestamp(self):
        content = self._merged_FifoDumpBinFileWrite__build_fifo_block_info(
            core_id=4, length=128
        )
        content += self._merged_FifoDumpBinFileWrite__build_fifo_timestamp_tlv()
        test_file = os.path.join(self.test_dir, "ts_test.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "4")
        fifo_dump.parse()
        result_dir = fifo_dump.write_result(self.test_dir)
        self.assertTrue(os.path.exists(result_dir))

    def test_write_index_dtype(self):
        content = self._merged_FifoDumpBinFileWrite__build_fifo_block_info(
            core_id=5, length=128
        )
        content += self._merged_FifoDumpBinFileWrite__build_fifo_tensor_tlv(
            desc=10, data_type=3, values=(1, 2)
        )
        test_file = os.path.join(self.test_dir, "dtype_test.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "5")
        fifo_dump.parse()
        fifo_dump.write_result(self.test_dir)
        fifo_dump.write_index_dtype(self.test_dir)
        json_file = os.path.join(self.test_dir, "dump_data", "index_dtype.json")
        self.assertTrue(os.path.exists(json_file))

    def test_show_print_with_content(self):
        content = self._merged_FifoDumpBinFileWrite__build_fifo_block_info(
            core_id=6, length=64
        )
        test_file = os.path.join(self.test_dir, "print_test.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "6")
        fifo_dump.parse()
        fifo_dump.show_print()

    def test_parse_fifo_file_with_simt_print_and_assert(self):
        content = self._merged_FifoDumpBinFileWrite__build_fifo_block_info(
            core_id=6, length=256
        )
        content += self._merged_FifoDumpBinFileWrite__build_fifo_simt_print_tlv(
            "simt_print=%d\n", 21
        )
        content += self._merged_FifoDumpBinFileWrite__build_fifo_simt_print_tlv(
            "simt_assert=%d\n", 1, tag=DumpType.SIMT_ASSERT_TYPE.value
        )
        test_file = os.path.join(self.test_dir, "simt_print_assert.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "6")
        fifo_dump.parse()
        core_content = fifo_dump.dump_core_contents[0]
        self.assertEqual(
            core_content.print_list, ["simt_print=21\n", "simt_assert=1\n"]
        )
        self.assertIn("4_5_6", core_content.simt_print_map)

    def test_write_result_simt_split_by_thread_id(self):
        content = self._merged_FifoDumpBinFileWrite__build_fifo_block_info(
            core_id=6, length=256
        )
        content += self._merged_FifoDumpBinFileWrite__build_fifo_simt_print_tlv(
            "simt_print=%d\n", 21
        )
        content += self._merged_FifoDumpBinFileWrite__build_fifo_simt_print_tlv(
            "simt_assert=%d\n", 1, tag=DumpType.SIMT_ASSERT_TYPE.value
        )
        test_file = os.path.join(self.test_dir, "asc_kernel_data_simt_6.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "simt", "6")
        fifo_dump.parse()
        result_dir = fifo_dump.write_result(self.test_dir)
        simt_txt = os.path.join(
            result_dir, "6", "asc_kernel_data_simt_6_thread_4_5_6.txt"
        )
        self.assertTrue(os.path.exists(simt_txt))
        with open(simt_txt, "r") as f:
            txt_content = f.read()
        self.assertEqual(txt_content, "simt_print=21\nsimt_assert=1\n")

    def _merged_DumpBinFileAdvanced__build_block_info(
        self,
        total_size=1024 * 1024,
        block_id=0,
        block_num=1,
        remain_size=0,
        magic=1520811213,
    ):
        return struct.pack(
            "iiiiiiQ", total_size, block_id, block_num, remain_size, magic, 0, 0
        )

    def _merged_DumpBinFileAdvanced__build_tlv(self, tag, value):
        return struct.pack(TLV.get_tl_format(), tag, len(value)) + value

    def test_parse_with_valid_block(self):
        block_info = self._merged_DumpBinFileAdvanced__build_block_info(
            total_size=256, remain_size=200
        )
        content = block_info + b"\x00" * 200
        test_file = os.path.join(self.test_dir, "valid_block.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        dump_file = DumpBinFile(test_file)
        dump_file.parse()
        self.assertEqual(len(dump_file.dump_core_contents), 1)

    def test_parse_with_invalid_block(self):
        block_info = self._merged_DumpBinFileAdvanced__build_block_info(magic=305419896)
        test_file = os.path.join(self.test_dir, "invalid_block.bin")
        with open(test_file, "wb") as f:
            f.write(block_info + b"\x00" * 1024 * 1024)
        dump_file = DumpBinFile(test_file)
        dump_file.parse()
        self.assertEqual(len(dump_file.dump_core_contents), 0)

    def test_write_index_dtype_with_data(self):
        block_info = self._merged_DumpBinFileAdvanced__build_block_info(
            total_size=256, remain_size=200
        )
        header = struct.pack("iiiiii", 0, 3, 5, 0, 0, 0)
        tensor_bytes = struct.pack("ii", 1, 2)
        tensor_tlv = self._merged_DumpBinFileAdvanced__build_tlv(
            DumpType.TENSOR_TYPE.value, header + tensor_bytes
        )
        content = block_info + tensor_tlv + b"\x00" * 200
        test_file = os.path.join(self.test_dir, "dtype.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        dump_file = DumpBinFile(test_file)
        dump_file.parse()
        self.assertIn(5, dump_file.index_dtype_dt)
        self.assertEqual(dump_file.index_dtype_dt[5], "int32")
        os.makedirs(os.path.join(self.test_dir, "dump_data"), exist_ok=True)
        dump_file.write_index_dtype(self.test_dir)
        json_file = os.path.join(self.test_dir, "dump_data", "index_dtype.json")
        self.assertTrue(os.path.exists(json_file))

    def test_dump_core_content_index_dtype_is_instance_local(self):
        first = DumpCoreContent()
        second = DumpCoreContent()

        first.index_dtype_dt[101] = "int32"

        self.assertIsNot(first.index_dtype_dt, second.index_dtype_dt)
        self.assertEqual(second.index_dtype_dt, {})

    def _merged_ParseDumpBin__build_fifo_block_info(self, core_id=0, length=64, flag=0):
        return struct.pack(
            FifoBlockInfo.get_format(),
            length,
            core_id,
            1,
            0,
            44678,
            flag,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        )

    def _merged_ParseDumpBin__build_legacy_block_info(
        self, total_size=1024 * 1024, block_id=0, remain_size=0
    ):
        return struct.pack(
            "iiiiiiQ", total_size, block_id, 1, remain_size, 1520811213, 0, 0
        )

    @patch("show_kernel_debug_data.dump_parser.FifoDumpBinFile")
    def test_parse_fifo_flow(self, fifo_cls):
        content = self._merged_ParseDumpBin__build_fifo_block_info(
            core_id=7, length=FifoBlockInfo.get_size()
        )
        test_file = os.path.join(self.test_dir, "asc_kernel_data_vec_7.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_inst = MagicMock()
        fifo_cls.return_value = fifo_inst
        dump_parser.parse_dump_bin(test_file, self.test_dir)
        fifo_cls.assert_called_once()
        fifo_inst.parse.assert_called_once()
        fifo_inst.write_result.assert_called_once()

    @patch("show_kernel_debug_data.dump_parser.FifoDumpBinFile")
    def test_parse_fifo_flow_with_simt_filename(self, fifo_cls):
        content = self._merged_ParseDumpBin__build_fifo_block_info(
            core_id=3, length=FifoBlockInfo.get_size()
        )
        test_file = os.path.join(self.test_dir, "asc_kernel_data_simt_3.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_inst = MagicMock()
        fifo_cls.return_value = fifo_inst
        dump_parser.parse_dump_bin(test_file, self.test_dir)
        fifo_cls.assert_called_once_with(os.path.abspath(test_file), "simt", "3")
        fifo_inst.parse.assert_called_once()

    @patch("show_kernel_debug_data.dump_parser.FifoDumpBinFile")
    def test_parse_fifo_flow_nonstandard_filename_core_type_from_flag(self, fifo_cls):
        content = self._merged_ParseDumpBin__build_fifo_block_info(
            core_id=9, length=FifoBlockInfo.get_size(), flag=1
        )
        test_file = os.path.join(self.test_dir, "random_name.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_inst = MagicMock()
        fifo_cls.return_value = fifo_inst
        dump_parser.parse_dump_bin(test_file, self.test_dir)
        fifo_cls.assert_called_once_with(os.path.abspath(test_file), "aiv", "9")
        fifo_inst.parse.assert_called_once()

    @patch("show_kernel_debug_data.dump_parser.FifoDumpBinFile")
    def test_parse_fifo_flow_nonstandard_filename_core_type_simt_from_flag(
        self, fifo_cls
    ):
        content = self._merged_ParseDumpBin__build_fifo_block_info(
            core_id=2, length=FifoBlockInfo.get_size(), flag=2
        )
        test_file = os.path.join(self.test_dir, "non_asc_name.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_inst = MagicMock()
        fifo_cls.return_value = fifo_inst
        dump_parser.parse_dump_bin(test_file, self.test_dir)
        fifo_cls.assert_called_once_with(os.path.abspath(test_file), "simt", "2")
        fifo_inst.parse.assert_called_once()

    @patch("show_kernel_debug_data.dump_parser.DumpBinFile")
    def test_parse_legacy_flow(self, legacy_cls):
        block_info = self._merged_ParseDumpBin__build_legacy_block_info(
            total_size=1024 * 1024
        )
        test_file = os.path.join(self.test_dir, "legacy.bin")
        with open(test_file, "wb") as f:
            f.write(block_info + b"\x00" * (1024 * 1024 - len(block_info)))
        legacy_inst = MagicMock()
        legacy_cls.return_value = legacy_inst
        dump_parser.parse_dump_bin(test_file, self.test_dir)
        legacy_cls.assert_called_once()
        legacy_inst.parse.assert_called_once()

    def test_parse_unknown_magic(self):
        content = struct.pack(
            FifoBlockInfo.get_format(), 64, 1, 1, 0, 65535, 0, 0, 0, 0, 0, 0, 0, 0, 0
        )
        test_file = os.path.join(self.test_dir, "unknown_magic.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        dump_parser.parse_dump_bin(test_file, self.test_dir)
        parser_dirs = [d for d in os.listdir(self.test_dir) if d.startswith("PARSER_")]
        self.assertEqual(len(parser_dirs), 1)

    def test_parse_bfloat16_data(self):
        tensor = FifoDumpTensor()
        tensor_bytes = struct.pack("HH", 16256, 16384)
        header = struct.pack(
            "IIIIHHI8III",
            0,
            27,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            len(tensor_bytes),
        )
        tlv = TLV(
            tag=DumpType.TENSOR_TYPE.value,
            length=len(header + tensor_bytes),
            value=header + tensor_bytes,
        )
        tensor.parse_from(tlv)
        self.assertEqual(len(tensor.dump_value), 2)

    def test_parse_unsupported_data_type(self):
        tensor = FifoDumpTensor()
        header = struct.pack(
            "IIIIHHI8III", 0, 99, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        )
        tlv = TLV(tag=DumpType.TENSOR_TYPE.value, length=len(header), value=header)
        tensor.parse_from(tlv)
        self.assertEqual(len(tensor.dump_value), 0)

    def test_incomplete_tlv_header(self):
        content = struct.pack(
            FifoBlockInfo.get_format(),
            FifoBlockInfo.get_size(),
            0,
            1,
            0,
            44678,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        )
        content += b"\x00\x00"
        test_file = os.path.join(self.test_dir, "incomplete_tlv.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "0")
        with self.assertRaisesRegex(
            RuntimeError, "FifoDumpBinFile: incomplete TLV header"
        ):
            fifo_dump.parse()

    def test_invalid_tlv_length(self):
        content = struct.pack(
            FifoBlockInfo.get_format(), 128, 0, 1, 0, 44678, 0, 0, 0, 0, 0, 0, 0, 0, 0
        )
        # length=0xFFFFFFFF validates unsigned TLV length overflow path after ii -> II change.
        content += struct.pack(TLV.get_tl_format(), 1, 0xFFFFFFFF)
        test_file = os.path.join(self.test_dir, "invalid_length.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "0")
        with self.assertRaisesRegex(RuntimeError, "TLV length overflow"):
            fifo_dump.parse()

    def test_tlv_length_overflow_without_value(self):
        content = struct.pack(
            FifoBlockInfo.get_format(), 128, 0, 1, 0, 44678, 0, 0, 0, 0, 0, 0, 0, 0, 0
        )
        content += struct.pack(TLV.get_tl_format(), 2, 16)
        test_file = os.path.join(self.test_dir, "length_overflow_without_value.bin")
        with open(test_file, "wb") as f:
            f.write(content)
        fifo_dump = FifoDumpBinFile(test_file, "vec", "0")
        with self.assertRaisesRegex(RuntimeError, "TLV length overflow"):
            fifo_dump.parse()

    # ---- Tests for error reporting improvements ----

    def test_parse_dump_bin_return_value(self):
        """parse_dump_bin returns 0 on success, 255 on failure (e.g. unknown magic)."""
        # success case
        content = self._merged_ParseDumpBin__build_fifo_block_info(
            core_id=0, length=FifoBlockInfo.get_size()
        )
        good_file = os.path.join(self.test_dir, "success.bin")
        with open(good_file, "wb") as f:
            f.write(content)
        self.assertEqual(dump_parser.parse_dump_bin(good_file, self.test_dir), 0)

        # failure case: unknown magic triggers RuntimeError caught by except
        bad_content = struct.pack(
            FifoBlockInfo.get_format(), 64, 1, 1, 0, 65535, 0, 0, 0, 0, 0, 0, 0, 0, 0
        )
        bad_file = os.path.join(self.test_dir, "bad_magic.bin")
        with open(bad_file, "wb") as f:
            f.write(bad_content)
        self.assertEqual(dump_parser.parse_dump_bin(bad_file, self.test_dir), 255)

        # failure case: file too small to contain a valid block
        tiny_file = os.path.join(self.test_dir, "tiny.bin")
        with open(tiny_file, "wb") as f:
            f.write(b"\xff\xff\xff\xff")
        self.assertEqual(dump_parser.parse_dump_bin(tiny_file, self.test_dir), 255)

    def test_non_ascii_path_raises_descriptive_error(self):
        """Non-ASCII paths should raise RuntimeError with a clear hint about encoding issues."""
        # input path
        chinese_input = "/tmp/中文目录/dump.bin"
        with patch("sys.argv", ["show_kernel_debug_data", chinese_input, "/tmp"]):
            with self.assertRaises(RuntimeError) as ctx:
                dump_parser.execute_parse()
            self.assertIn("non-ASCII", str(ctx.exception))

        # output path
        ascii_input = os.path.join(self.test_dir, "test.bin")
        with open(ascii_input, "wb") as f:
            f.write(b"\x00")
        chinese_output = "/tmp/输出目录"
        with patch("sys.argv", ["show_kernel_debug_data", ascii_input, chinese_output]):
            with self.assertRaises(RuntimeError) as ctx:
                dump_parser.execute_parse()
            self.assertIn("non-ASCII", str(ctx.exception))

    def test_help_flag_returns_zero(self):
        """execute_parse with -h should return 0."""
        with patch("sys.argv", ["show_kernel_debug_data", "-h"]):
            self.assertEqual(dump_parser.execute_parse(), 0)

    def test_execute_parse_return_value(self):
        """execute_parse returns 0 on success, 255 when any file fails to parse."""
        # success case
        ok_dir = os.path.join(self.test_dir, "ok_bins")
        os.makedirs(ok_dir)
        content = self._merged_ParseDumpBin__build_fifo_block_info(
            core_id=0, length=FifoBlockInfo.get_size()
        )
        with open(os.path.join(ok_dir, "good.bin"), "wb") as f:
            f.write(content)
        with patch("sys.argv", ["show_kernel_debug_data", ok_dir, self.test_dir]):
            self.assertEqual(dump_parser.execute_parse(), 0)

        # failure case
        fail_dir = os.path.join(self.test_dir, "fail_bins")
        os.makedirs(fail_dir)
        with open(os.path.join(fail_dir, "bad.bin"), "wb") as f:
            f.write(b"\xff\xff\xff\xff")
        with patch("sys.argv", ["show_kernel_debug_data", fail_dir, self.test_dir]):
            self.assertEqual(dump_parser.execute_parse(), 255)


if __name__ == "__main__":
    unittest.main()
