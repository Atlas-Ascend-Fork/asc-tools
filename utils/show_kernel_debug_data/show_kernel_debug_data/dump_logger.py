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

import logging
import os

LOG_LEVEL_DICT = {
    "0": logging.DEBUG,
    "1": logging.INFO,
    "2": logging.WARNING,
    "3": logging.ERROR,
}

OUT_LOG_FORMAT = logging.Formatter("%(asctime)s %(message)s")
FILE_LOG_FORMAT = logging.Formatter(
    "[%(levelname)s] show_kernel_debug_data [%(process)d] %(asctime)s %(message)s"
)

DEFAULT_LOG_FILE = "dump_parser.log"


class DumpParserLog:
    def __init__(self) -> None:
        self.log_level = logging.INFO
        self.logger = logging.getLogger("show_kernel_debug_data_log")
        self.logger.setLevel(self.log_level)

        # 设置打屏日志， WARNING以上, 不能改变
        self.out_handler = logging.StreamHandler()
        self.out_handler.setFormatter(OUT_LOG_FORMAT)
        self.out_handler.setLevel(logging.WARNING)
        self.logger.addHandler(self.out_handler)

        # 文件日志, 通过 _set_log_file 接口设置
        self.log_file = ""
        self.file_handler = None
        self._set_log_file(os.path.join(os.getcwd(), DEFAULT_LOG_FILE), reset=False)

    def get_log_file(self) -> str:
        return self.log_file

    def _set_log_file(self, log_file, reset=True):
        if self.file_handler:
            self.logger.removeHandler(self.file_handler)

        log_path = os.path.dirname(log_file)
        if log_path and not os.path.isdir(log_path):
            os.makedirs(log_path)

        if reset and os.path.exists(log_file):
            os.remove(log_file)
        self.log_file = log_file
        self.file_handler = logging.FileHandler(
            self.log_file, encoding="utf-8", delay=True
        )
        self.file_handler.setFormatter(FILE_LOG_FORMAT)
        self.file_handler.setLevel(self.log_level)
        self.logger.addHandler(self.file_handler)

    def set_log_level(self, log_level: str = "3") -> None:
        self.log_level = LOG_LEVEL_DICT.get(log_level, logging.ERROR)
        self.logger.setLevel(self.log_level)
        if self.file_handler:
            self.file_handler.setLevel(self.log_level)

    def set_log_file(self, log_file=""):
        file_name = os.path.basename(log_file)
        if not file_name:
            file_name = DEFAULT_LOG_FILE

        log_path = os.path.dirname(log_file)
        if not log_path:
            log_path = os.getcwd()

        log_file = os.path.join(log_path, file_name)
        print("log file saves to ", log_file)
        self._set_log_file(log_file)

    def info(self, msg: object, *args: object, **kwargs: object) -> None:
        """
        Auto tune pregress info
        :param msg:
        :param args:
        :param kwargs:
        :return:
        """
        self.logger.info(f"[INFO]: {msg}", *args, **kwargs)

    def debug(self, msg: object, *args: object, **kwargs: object) -> None:
        """
        debug msg
        :param msg:
        :param args:
        :param kwargs:
        :return:
        """
        self.logger.debug(f"[DEBUG]: {msg}", *args, **kwargs)

    def warning(self, msg: object, *args: object, **kwargs: object) -> None:
        """
        warning msg
        :param msg:
        :param args:
        :param kwargs:
        :return:
        """
        self.logger.warning(f"[WARNING]: {msg}", *args, **kwargs)

    def error(self, msg: object, *args: object, **kwargs: object) -> None:
        """
        error msg
        :param msg:
        :param args:
        :param kwargs:
        :return:
        """
        self.logger.error(f"[ERROR]: {msg}", *args, **kwargs)


DUMP_PARSER_LOG = DumpParserLog()
