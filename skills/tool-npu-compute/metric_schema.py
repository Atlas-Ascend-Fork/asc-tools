# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Field definitions for current and historical PMU report schemas."""


def legacy_metric_fields(section, fields):
    legacy_names = section.get("legacy_field_names", {})
    return [
        {**field, "name": legacy_names.get(field["name"], field["name"])}
        for field in fields
        if legacy_names.get(field["name"], field["name"]) is not None
    ]


def metric_fields_for_names(section, names, excluded=()):
    fields = [field for field in section["fields"] if field["name"] not in excluded]
    if "legacy_field_names" in section:
        legacy = legacy_metric_fields(section, fields)
        if names == [field["name"] for field in legacy]:
            return legacy
    return fields
