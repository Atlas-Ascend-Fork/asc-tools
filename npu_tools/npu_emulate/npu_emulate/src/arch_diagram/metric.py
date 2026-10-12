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

from math import isclose
from dataclasses import dataclass
from collections import defaultdict
from typing import Dict

from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.common.biprof_enum import ArchName
from npu_emulate.src.arch_diagram.constants import MEMORY_BANDWIDTH_V100


@dataclass
class MetricData:
    formatted_value: str
    relative_tp: float = 0.0


class MetricFormat:
    @staticmethod
    def format_time(raw_value: float) -> MetricData:
        value = f"{raw_value:.2f} us"
        return MetricData(value)

    @staticmethod
    def format_throughput(raw_value: float) -> MetricData:
        if not isclose(raw_value, 0.0) and raw_value < 0.01:
            value = "<0.01 GB/s"
        else:
            value = f"{raw_value:.2f} GB/s"
        return MetricData(value)

    @staticmethod
    def format_throughput_with_peak(raw_value: float, peak_value: float) -> MetricData:
        metric_data = MetricFormat.format_throughput(raw_value)
        if peak_value == 0.0:
            return metric_data
        metric_data.relative_tp = (raw_value / peak_value) * 100
        return metric_data

    @staticmethod
    def format_count(raw_value: float) -> MetricData:
        value = f"{int(raw_value)}"
        return MetricData(value)

    @staticmethod
    def format_usage(raw_value: float) -> MetricData:
        value = f"{raw_value:.2f}%"
        return MetricData(value)


class Metric:
    METRICS_FORMATTER = {
        "time": MetricFormat.format_time,
        "tp": MetricFormat.format_throughput,
        "count": MetricFormat.format_count,
        "usage": MetricFormat.format_usage,
    }
    name: str
    data: MetricData

    def __init__(self, name: str, format_type: str = ""):
        self.name = name
        self.data = MetricData("No formatter")

        self._strip_name = name
        if name.endswith("_gbs"):
            self._strip_name = name[:-4]
        elif name.endswith("_ratio"):
            self._strip_name = name[:-6]
        self._format_type = format_type
        self._formatter = self.METRICS_FORMATTER.get(format_type)

    def format(self, raw_value: float) -> MetricData:
        # specific formatter for throughput metrics with known peaks
        bw_peaks = self.get_bw_peaks()
        if self._format_type == "tp" and self._strip_name in bw_peaks:
            peak_value = bw_peaks.get(self._strip_name)
            if peak_value is None:
                peak_value = 0.0
            return MetricFormat.format_throughput_with_peak(raw_value, peak_value)
        # common formatters for other metrics
        if self._formatter is not None:
            return self._formatter(raw_value)

        # impossible case to make mypy happy
        return self.data

    def get_bw_peaks(self) -> Dict[str, float]:
        if biprof_ctx.arch == ArchName.V100:
            return MEMORY_BANDWIDTH_V100
        return defaultdict(float)
