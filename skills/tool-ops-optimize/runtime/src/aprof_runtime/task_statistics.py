# Copyright (c) 2026 AProf contributors.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
"""Predeclared, bounded outlier exclusion for 20 complete Task calls."""

import math
import statistics

from .errors import ContractError

BOUNDED_OUTLIER_POLICY = "bounded_outlier_v1"
NORMAL_MAD_SCALE = 1.482602218505602
MAX_EXCLUDED = 2


def bounded_statistics(values):
    """One symmetric pass; excess outliers invalidate the capture without trimming."""
    if len(values) != 20 or any(
        type(x) not in (int, float) or not math.isfinite(x) or x <= 0 for x in values
    ):
        raise ContractError("异常值判据需要完整的 20 个正有限 Task 时长")
    values = list(values)
    center = statistics.median(values)
    mad = statistics.median(abs(x - center) for x in values)
    threshold = max(3 * NORMAL_MAD_SCALE * mad, 0.10 * center)
    outliers = [i + 1 for i, x in enumerate(values) if abs(x - center) > threshold]
    exceeded = len(outliers) > MAX_EXCLUDED
    excluded = [] if exceeded else outliers
    retained = [x for i, x in enumerate(values, 1) if i not in excluded]
    return {
        "policy": BOUNDED_OUTLIER_POLICY,
        "raw_values_us": values,
        "raw_median_us": center,
        "raw_cv": statistics.stdev(values) / statistics.fmean(values),
        "mad_us": mad,
        "outlier_threshold_us": threshold,
        "outlier_indices": outliers,
        "excluded_indices": excluded,
        "retained_count": len(retained),
        "median_us": statistics.median(retained),
        "cv": statistics.stdev(retained) / statistics.fmean(retained),
        "exclusion_limit_exceeded": exceeded,
    }
