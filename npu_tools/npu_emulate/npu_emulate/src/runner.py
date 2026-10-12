# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

import abc
import logging as log
import os
import shutil
import traceback
from typing import List, Optional

from npu_emulate.src.common.biprof_enum import AnalysisType, BiprofMode
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.common.args import BiProfArgParser
from npu_emulate.src.backend.base.data_processors_base import IBaseReporter
from npu_emulate.src.backend.reporters import HotspotsDbOnlyReporter


class IBiProfBaseMode(abc.ABC):
    @abc.abstractmethod
    def init(self) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def run(self) -> int:
        raise NotImplementedError


class BiProfReportMode(IBiProfBaseMode):
    def __init__(self):
        self._launcher = None

    def init(self) -> int:
        self._launcher = BiProfReportSimu()
        ret = self._launcher.init()
        if ret:
            return ret
        return 0

    def run(self) -> int:
        ret = self._launcher.run()
        return ret if isinstance(ret, int) else 1


class IBiProfLauncher(abc.ABC):
    def __init__(self) -> None:
        self._analysis_name = biprof_ctx.analysis_type

    @abc.abstractmethod
    def init(self) -> int:
        pass

    @abc.abstractmethod
    def run(self) -> int:
        pass


class IBiProfReportLauncher(IBiProfLauncher):
    def __init__(self) -> None:
        super().__init__()
        self._reporter: Optional[IBaseReporter] = None

    @abc.abstractmethod
    def get_reporter(self) -> Optional[IBaseReporter]:
        pass

    def init(self) -> int:
        self._reporter = self.get_reporter()
        if self._reporter is None:
            return 1
        return self._reporter.init()

    def run(self) -> int:
        if self._reporter is None:
            return 1
        ret = self._reporter.process()
        if ret != 0:
            log.error("Report generation failed!")
        return ret


class BiProfReportSimu(IBiProfReportLauncher):
    def get_reporter(self) -> Optional[IBaseReporter]:
        reporter: Optional[IBaseReporter] = None
        if self._analysis_name == AnalysisType.HOTSPOTS:
            reporter = HotspotsDbOnlyReporter()
        else:
            log.critical(
                f"Simulation report is supported "
                f"only for {AnalysisType.HOTSPOTS} analysis type"
            )
            return None
        final_logger.add_general_info(f"Analysis type: {self._analysis_name}")
        return reporter


class BiProfRunner:
    @staticmethod
    def _get_mode() -> Optional[IBiProfBaseMode]:
        mode = biprof_ctx.mode
        if mode == BiprofMode.REPORT:
            return BiProfReportMode()
        log.critical("Unrecognized BiProf mode!")
        return None

    @staticmethod
    def init(input_args: List[str]) -> int:
        arg_parser = BiProfArgParser()
        args = arg_parser.parse(input_args)

        if args is None:
            return 1

        log.basicConfig(level=log.__dict__[args.log_level.upper()])
        if args.mode == BiprofMode.REPORT:
            args.output_dir = os.path.abspath(args.output)
            if os.path.exists(args.output_dir):
                shutil.rmtree(args.output_dir)
            os.makedirs(args.output_dir)
            fh = log.FileHandler(f"{args.output_dir}/{os.path.basename(__file__)}.log")
            fh.setLevel(log.DEBUG)
            root = log.getLogger()
            root.addHandler(fh)
            for h in root.handlers:
                if isinstance(h, log.StreamHandler) and not isinstance(
                    h, log.FileHandler
                ):
                    root.removeHandler(h)
        biprof_ctx.init(args)
        return 0

    @classmethod
    def run(cls, args):
        ret = cls.init(args)
        try:
            with StageReporter("BiProf", hidden=True):
                if ret != 0:
                    return ret
                with StageReporter("BiProf initialization"):
                    biprof = cls._get_mode()
                    if biprof is None:
                        return 1
                    ret = biprof.init()
                    if ret != 0:
                        return ret
                ret = biprof.run()
                final_logger.report(ret)
            StageReporter.dump_perf_table()
            return ret
        except Exception as e:
            if biprof_ctx.log_level.upper() == log.getLevelName(log.DEBUG):
                log.error(traceback.format_exc())
            else:
                log.error(f"biprof failed with the following error: {e}")
            return 1
