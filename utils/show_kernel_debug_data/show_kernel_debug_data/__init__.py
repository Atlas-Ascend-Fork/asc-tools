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
from show_kernel_debug_data.dump_parser import (
    parse_dump_bin,
    _collect_bin_files,
    _make_parser_output_dir,
)
from show_kernel_debug_data.dump_logger import DUMP_PARSER_LOG


def show_kernel_debug_data(bin_file_path: str, output_path: str = "./"):
    if not bin_file_path or not os.path.exists(bin_file_path):
        raise RuntimeError(
            f"({bin_file_path}) file does not exist or permission denied!!!"
        )
    if not os.path.isfile(bin_file_path) and not os.path.isdir(bin_file_path):
        raise RuntimeError(f"({bin_file_path}) is neither a file nor a directory!!!")

    if not output_path:
        raise RuntimeError(
            f"({output_path}) directory does not exist or permission denied!!!"
        )
    if os.path.exists(output_path):
        if not os.path.isdir(output_path):
            raise RuntimeError(f"({output_path}) is not a directory!!!")
    else:
        try:
            os.makedirs(output_path, exist_ok=True)
        except OSError as err:
            raise RuntimeError(
                f"({output_path}) directory does not exist or permission denied!!!"
            ) from err

    dump_bins = _collect_bin_files(bin_file_path)
    if not dump_bins:
        raise RuntimeError(f"({bin_file_path}) does not contain any .bin file!!!")

    if os.path.isdir(bin_file_path):
        parser_output_dir = _make_parser_output_dir(output_path)
        DUMP_PARSER_LOG.set_log_file(os.path.join(parser_output_dir, "parser.log"))
        DUMP_PARSER_LOG.set_log_level(os.environ.get("ASCEND_GLOBAL_LOG_LEVEL", "3"))
        for dump_bin in dump_bins:
            parse_dump_bin(
                dump_bin,
                output_path,
                parse_output_dir=parser_output_dir,
                init_logger=False,
            )
        DUMP_PARSER_LOG.show_log_file()
    else:
        parse_dump_bin(dump_bins[0], output_path)
