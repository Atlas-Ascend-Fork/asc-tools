#!/usr/bin/env python3
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import argparse
import json
import logging
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cli_support import add_format_argument, configure_cli_loggers, emit_result

LOGGER, OUTPUT_LOGGER = configure_cli_loggers(__name__)

CATALOG_PATH = (
    Path(__file__).resolve().parents[1] / "assets" / "hardware-info-catalog.json"
)


class InspectionError(Exception):
    pass


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="检查并解释 npu-compute HardwareInfo.jsonl。"
    )
    parser.add_argument(
        "jsonl_path", type=Path, help="需要检查的 HardwareInfo JSONL 文件"
    )
    add_format_argument(parser)
    return parser.parse_args()


def load_catalog():
    with CATALOG_PATH.open(encoding="utf-8") as input_file:
        return json.load(input_file)


def load_records(path, categories):
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise InspectionError(f"读取 HardwareInfo 失败：{error}") from error

    records = []
    for line_number, line in enumerate(lines, start=1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise InspectionError(
                f"第 {line_number} 行 JSON 格式错误：{error.msg}"
            ) from error
        if not isinstance(record, dict):
            raise InspectionError(f"第 {line_number} 行必须是 JSON 对象")
        if "category" not in record:
            raise InspectionError(f"第 {line_number} 行缺少 category")
        category = record["category"]
        if not isinstance(category, str) or not category:
            raise InspectionError(f"第 {line_number} 行 category 必须是非空字符串")
        if category not in categories:
            raise InspectionError(f"未知类别：{category}")
        records.append(record)
    if not records:
        raise InspectionError(f"缺少类别：{', '.join(categories)}")
    return records


def validate_type(category, field, value):
    name = field["name"]
    json_type = field["json_type"]
    if json_type == "string":
        if not isinstance(value, str):
            raise InspectionError(f"{category}.{name} 类型应为 string")
        if value == "" and field.get("allow_empty") is True:
            return
        if not value.strip():
            raise InspectionError(f"{category}.{name} 不能为空")
        return

    if json_type == "integer":
        if not isinstance(value, int) or isinstance(value, bool):
            raise InspectionError(f"{category}.{name} 类型应为 integer")
    elif json_type == "number":
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            raise InspectionError(f"{category}.{name} 类型应为 number")
    else:
        raise InspectionError(f"{category}.{name} 使用未知目录类型：{json_type}")
    if not math.isfinite(value) or value < 0:
        raise InspectionError(f"{category}.{name} 必须是有限非负数")


def inspect(path, catalog):
    catalog_categories = {
        category["name"]: category for category in catalog["categories"]
    }
    device_id = catalog["device_id"]
    device_id_field = device_id["field"]
    device_id_name = device_id_field["name"]
    records = load_records(path, catalog_categories)
    snapshot_size = len(catalog_categories)
    seen_devices = set()
    output_categories = []
    for start in range(0, len(records), snapshot_size):
        snapshot = {}
        identifiers = set()
        for record in records[start : start + snapshot_size]:
            category_name = record["category"]
            if category_name in snapshot:
                raise InspectionError(f"重复类别：{category_name}")
            snapshot[category_name] = record
            if category_name in device_id["categories"]:
                identifier = None
                if device_id_name in record:
                    identifier = record[device_id_name]
                    validate_type(category_name, device_id_field, identifier)
                identifiers.add(identifier)

        if len(identifiers) > 1:
            raise InspectionError("同一快照的 Device Id 不一致")
        identifier = next(iter(identifiers), None)
        if identifier in seen_devices:
            if identifier is None:
                raise InspectionError("重复类别：Host Info")
            raise InspectionError(f"重复设备快照：Device Id {identifier}")
        seen_devices.add(identifier)
        if len(seen_devices) > 1 and None in seen_devices:
            raise InspectionError("多设备快照缺少 Device Id")

        missing = [
            category for category in catalog_categories if category not in snapshot
        ]
        if missing:
            prefix = "" if identifier is None else f"Device Id {identifier} "
            raise InspectionError(f"{prefix}缺少类别：{', '.join(missing)}")
        if len(records) > snapshot_size and list(snapshot) != list(catalog_categories):
            raise InspectionError(f"Device Id {identifier} 类别顺序不一致")

        for category_name, category in catalog_categories.items():
            fields = category["fields"]
            if identifier is not None and category_name in device_id["categories"]:
                fields = [fields[0], device_id_field, *fields[1:]]
            output_categories.append(
                {
                    "name": category_name,
                    "description": category["description"],
                    "fields": inspect_fields(
                        category_name, snapshot[category_name], fields, device_id_name
                    ),
                }
            )
    return {
        "file": str(path),
        "record_count": len(output_categories),
        "categories": output_categories,
    }


def inspect_fields(category_name, record, fields, device_id_name):
    expected_names = [field["name"] for field in fields]
    actual_names = list(record)
    unknown = [name for name in actual_names if name not in expected_names]
    missing = [name for name in expected_names if name not in actual_names]
    errors = []
    if unknown:
        errors.append(f"{category_name} 未知字段：{', '.join(unknown)}")
    if missing:
        errors.append(f"{category_name} 缺少字段：{', '.join(missing)}")
    if errors:
        raise InspectionError("\n".join(errors))
    if actual_names != expected_names:
        raise InspectionError(f"{category_name} 字段顺序不一致")

    output_fields = []
    for field in fields:
        name = field["name"]
        value = record[name]
        if name != device_id_name:
            validate_type(category_name, field, value)
        output_fields.append(
            {
                "name": name,
                "value": value,
                "json_type": field["json_type"],
                "meaning": field["meaning"],
                "unit": field["unit"],
                "variability": field["variability"],
                "analysis": field["analysis"],
                "notes": field["notes"],
            }
        )
    return output_fields


def escape_table_cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def render_markdown(result):
    lines = [
        "# HardwareInfo 检查",
        "",
        f"- **文件：** `{result['file']}`",
        f"- **记录数：** {result['record_count']}",
    ]
    for category in result["categories"]:
        lines.extend(
            (
                "",
                f"## {category['name']}",
                "",
                category["description"],
                "",
                "| 字段 | 实际值 | 类型 | 含义 | 单位 | 属性 | 分析用途 | 注意事项 |",
                "|---|---|---|---|---|---|---|---|",
            )
        )
        for field in category["fields"]:
            values = (
                f"`{field['name']}`",
                field["value"],
                field["json_type"],
                field["meaning"],
                field["unit"],
                field["variability"],
                field["analysis"],
                field["notes"],
            )
            lines.append(
                "| " + " | ".join(escape_table_cell(value) for value in values) + " |"
            )
    return "\n".join(lines)


def main():
    logging.basicConfig(format="%(message)s")
    arguments = parse_arguments()
    try:
        result = inspect(arguments.jsonl_path, load_catalog())
    except (InspectionError, json.JSONDecodeError, OSError) as error:
        LOGGER.error("错误：%s", error)
        return 2

    emit_result(OUTPUT_LOGGER, arguments.format, result, render_markdown)
    return 0


if __name__ == "__main__":
    sys.exit(main())
