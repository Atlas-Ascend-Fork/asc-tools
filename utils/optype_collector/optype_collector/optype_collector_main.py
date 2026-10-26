#!/usr/bin/env python3
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
"""Collect and detect duplicated OpType definitions in CANN OPP packages."""

import argparse
import json
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple

BUILTIN = "builtin"
CUSTOM = "custom"
CUSTOM_OPP_ENV = "ASCEND_CUSTOM_OPP_PATH"
ASCEND_HOME_ENV = "ASCEND_HOME_PATH"
CANN_ENV_ERROR = "Error: CANN environment variables are not configured."
CANN_ENV_SOURCE_HINT = "Please execute: source <CANN_install_path>/cann/set_env.sh"


@dataclass
class OpTypeSource:
    """One OpType scan source."""

    source_type: str
    soc: str
    matched_soc: str
    root_path: Path
    vendor_name: Optional[str] = None
    config_files: List[Path] = field(default_factory=list)
    optypes: Set[str] = field(default_factory=set)
    warnings: List[str] = field(default_factory=list)
    errors: List[str] = field(default_factory=list)

    @property
    def status(self) -> str:
        if self.errors:
            return "ERROR"
        if self.warnings:
            return "WARN"
        return "OK"

    @property
    def identity(self) -> Tuple[str, str, str, str]:
        return (
            self.source_type,
            self.vendor_name or "",
            self.matched_soc,
            str(self.root_path),
        )


@dataclass
class ScanResult:
    """Full scan result."""

    user_soc: str
    soc_names: List[str]
    ascend_home_path: Optional[Path]
    custom_opp_path: Optional[str]
    builtin_sources: List[OpTypeSource] = field(default_factory=list)
    custom_sources: List[OpTypeSource] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    errors: List[str] = field(default_factory=list)


@dataclass
class ConflictGroup:
    """One duplicated OpType conflict group."""

    conflict_type: str
    left: OpTypeSource
    right: OpTypeSource
    optypes: List[str]


@dataclass
class ConflictReport:
    """Conflict detection report."""

    custom_builtin: List[ConflictGroup] = field(default_factory=list)
    custom_custom: List[ConflictGroup] = field(default_factory=list)

    @property
    def has_conflicts(self) -> bool:
        return bool(self.custom_builtin or self.custom_custom)


@dataclass
class SocNameMap:
    """Map public SoC versions to internal OPP config directory names."""

    external_to_short: Dict[str, List[str]] = field(default_factory=dict)
    external_lower_to_short: Dict[str, List[str]] = field(default_factory=dict)
    short_to_external: Dict[str, List[str]] = field(default_factory=dict)


def _append_unique(items: List[str], item: str) -> None:
    if item and item not in items:
        items.append(item)


def _read_platform_config_soc_names(
    config_file: Path,
) -> Tuple[Optional[str], Optional[str]]:
    external_soc = None
    short_soc = None
    try:
        with config_file.open("r", encoding="utf-8") as file_obj:
            for raw_line in file_obj:
                line = raw_line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, value = [part.strip() for part in line.split("=", 1)]
                lowered_key = key.lower()
                if lowered_key == "soc_version":
                    external_soc = value
                elif lowered_key == "short_soc_version":
                    short_soc = value
    except (OSError, UnicodeDecodeError):
        return None, None
    return external_soc, short_soc


def _platform_config_dirs(ascend_home_path: Path) -> List[Path]:
    """Return platform_config directories without assuming a fixed CPU architecture."""
    candidates = []
    for child in sorted(ascend_home_path.glob("*-linux")):
        platform_config_dir = child / "data" / "platform_config"
        if platform_config_dir.is_dir():
            candidates.append(platform_config_dir)
    return candidates


def load_soc_name_map(ascend_home_path: Optional[Path]) -> SocNameMap:
    """Load public SoC name and internal short-name mapping from CANN platform_config."""
    name_map = SocNameMap()
    if not ascend_home_path:
        return name_map
    for platform_config_dir in _platform_config_dirs(ascend_home_path):
        for config_file in sorted(platform_config_dir.glob("*.ini")):
            external_soc, short_soc = _read_platform_config_soc_names(config_file)
            if not external_soc or not short_soc:
                continue
            short_key = short_soc.lower()
            _append_unique(
                name_map.external_to_short.setdefault(external_soc, []), short_key
            )
            _append_unique(
                name_map.external_lower_to_short.setdefault(external_soc.lower(), []),
                short_key,
            )
            _append_unique(
                name_map.short_to_external.setdefault(short_key, []), external_soc
            )
    return name_map


def _expand_internal_soc_names(soc_names: Iterable[str]) -> List[str]:
    alias_map = {
        "ascend950": ["ascend950", "ascend910_95"],
    }
    expanded_names = []
    for soc_name in soc_names:
        for expanded_name in alias_map.get(soc_name, [soc_name]):
            _append_unique(expanded_names, expanded_name)
    return expanded_names


def expand_soc_aliases(
    soc_version: str, soc_name_map: Optional[SocNameMap] = None
) -> List[str]:
    """Resolve SoC input to OPP config directory names."""
    normalized_soc = soc_version.lower()
    if soc_version == normalized_soc:
        return _expand_internal_soc_names([normalized_soc])
    if soc_name_map:
        mapped_names = soc_name_map.external_to_short.get(soc_version)
        if not mapped_names:
            mapped_names = soc_name_map.external_lower_to_short.get(normalized_soc)
        if mapped_names:
            return _expand_internal_soc_names(mapped_names)
    return []


def _read_json_file(json_file: Path, source: OpTypeSource) -> Optional[Any]:
    try:
        with json_file.open("r", encoding="utf-8") as file_obj:
            return json.load(file_obj)
    except Exception as err:
        source.warnings.append(
            "Failed to parse JSON: {}; reason: {}".format(json_file, err)
        )
        return None


def _looks_like_op_type(name: str) -> bool:
    if not name or name.startswith("_"):
        return False
    lowered = name.lower()
    non_op_keys = {
        "bininfo",
        "bin_info",
        "supportinfo",
        "support_info",
        "opinfo",
        "op_info",
        "oplist",
        "op_list",
        "ops",
        "op",
        "socversion",
        "soc_version",
        "version",
        "platform",
        "impl_path",
        "dynamic_compile_static",
        "static_compile",
        "dynamic_compile",
        "simplifiedkeymode",
        "simplified_key_mode",
        "computeunit",
        "compute_unit",
        "opfile",
        "op_file",
        "jsonfilepath",
        "json_file_path",
        "kernelname",
        "kernel_name",
        "input",
        "output",
        "attr",
        "attrs",
        "dtype",
        "format",
        "precision_reduce",
        "enable_vector_core",
    }
    return lowered not in non_op_keys


def _collect_optypes_from_json(data: Any) -> Set[str]:
    optypes = set()
    container_keys = {
        "ops",
        "opList",
        "op_list",
        "op_info",
        "opInfo",
        "binInfo",
        "bin_info",
    }
    explicit_optype_keys = {
        "opType",
        "op_type",
        "opTypeName",
        "op_type_name",
        "opName",
        "op_name",
    }

    def visit(obj: Any, parent_key: Optional[str] = None) -> None:
        if isinstance(obj, dict):
            for key, value in obj.items():
                if key in explicit_optype_keys:
                    if isinstance(value, str) and value:
                        optypes.add(value)
                    continue
                if key == "name" and parent_key in container_keys:
                    if isinstance(value, str) and value:
                        optypes.add(value)
                    continue

                key_is_optype = (
                    isinstance(key, str)
                    and isinstance(value, (dict, list))
                    and (parent_key is None or parent_key in container_keys)
                    and _looks_like_op_type(key)
                )
                if key_is_optype:
                    optypes.add(key)
                    visit(value, key)
                elif isinstance(value, (dict, list)):
                    visit(value, key)
        elif isinstance(obj, list):
            for item in obj:
                visit(item, parent_key)

    visit(data)
    return optypes


def _scan_config_dir(
    source_type: str,
    soc: str,
    matched_soc: str,
    root_path: Path,
    vendor_name: Optional[str] = None,
) -> OpTypeSource:
    source = OpTypeSource(
        source_type=source_type,
        soc=soc,
        matched_soc=matched_soc,
        vendor_name=vendor_name,
        root_path=root_path,
    )
    if not root_path.exists():
        source.errors.append("Path does not exist: {}".format(root_path))
        return source
    if not root_path.is_dir():
        source.errors.append("Scan path is not a directory: {}".format(root_path))
        return source

    json_files = sorted(root_path.rglob("*.json"))
    source.config_files = json_files
    if not json_files:
        source.warnings.append("No JSON config files found: {}".format(root_path))
        return source

    for json_file in json_files:
        data = _read_json_file(json_file, source)
        if data is None:
            continue
        source.optypes.update(_collect_optypes_from_json(data))
    if not source.optypes:
        source.warnings.append("No OpTypes parsed from: {}".format(root_path))
    return source


def _source_merge_key(source: OpTypeSource) -> Tuple[str, str, str]:
    """Return package identity, ignoring alias SoC directory names under the same package."""
    return (source.source_type, source.vendor_name or "", str(source.root_path.parent))


def _merge_matched_soc(left: str, right: str) -> str:
    soc_names = []
    for item in (left, right):
        for soc_name in item.split(","):
            soc_name = soc_name.strip()
            if soc_name and soc_name not in soc_names:
                soc_names.append(soc_name)
    return ",".join(soc_names)


def _merge_config_files(left: List[Path], right: Iterable[Path]) -> List[Path]:
    """Merge config file lists while preserving order and removing duplicate paths."""
    merged_files = list(left)
    seen = {str(path) for path in merged_files}
    for path in right:
        path_key = str(path)
        if path_key not in seen:
            merged_files.append(path)
            seen.add(path_key)
    return merged_files


def _deduplicate_sources(sources: Iterable[OpTypeSource]) -> List[OpTypeSource]:
    merged: Dict[Tuple[str, str, str], OpTypeSource] = {}
    for source in sources:
        key = _source_merge_key(source)
        if key not in merged:
            source.config_files = _merge_config_files([], source.config_files)
            merged[key] = source
            continue
        exist = merged[key]
        exist.matched_soc = _merge_matched_soc(exist.matched_soc, source.matched_soc)
        exist.config_files = _merge_config_files(
            exist.config_files, source.config_files
        )
        exist.optypes.update(source.optypes)
        exist.warnings.extend(source.warnings)
        exist.errors.extend(source.errors)
    return list(merged.values())


def _candidate_builtin_dirs(
    ascend_home_path: Path, soc_names: Sequence[str]
) -> List[Tuple[str, Path]]:
    base = (
        ascend_home_path / "opp" / "built-in" / "op_impl" / "ai_core" / "tbe" / "config"
    )
    candidates = []
    for soc_name in soc_names:
        candidates.append((soc_name, base / soc_name))
        if base.exists():
            for child in sorted(base.iterdir()):
                if child.is_dir() and child.name != soc_name:
                    candidates.append((soc_name, child / soc_name))
    return candidates


def collect_builtin_optypes(
    ascend_home_path: Path, user_soc: str, soc_names: Sequence[str]
) -> List[OpTypeSource]:
    sources = []
    for matched_soc, path in _candidate_builtin_dirs(ascend_home_path, soc_names):
        if path.exists():
            sources.append(_scan_config_dir(BUILTIN, user_soc, matched_soc, path))
    return _deduplicate_sources(sources)


def _vendor_dirs(
    vendors_root: Path, soc_names: Sequence[str]
) -> List[Tuple[str, str, Path]]:
    candidates = []
    if not vendors_root.exists():
        return candidates
    for vendor_dir in sorted(vendors_root.iterdir()):
        if not vendor_dir.is_dir():
            continue
        for soc_name in soc_names:
            candidates.append(
                (
                    vendor_dir.name,
                    soc_name,
                    vendor_dir / "op_impl" / "ai_core" / "tbe" / "config" / soc_name,
                )
            )
    return candidates


def _custom_opp_dirs(
    custom_root: Path, soc_names: Sequence[str]
) -> List[Tuple[str, str, Path]]:
    candidates = []
    for soc_name in soc_names:
        direct_path = custom_root / "op_impl" / "ai_core" / "tbe" / "config" / soc_name
        candidates.append((custom_root.name, soc_name, direct_path))
    if not custom_root.exists():
        return candidates
    for vendor_dir in sorted(custom_root.iterdir()):
        if not vendor_dir.is_dir():
            continue
        for soc_name in soc_names:
            candidates.append(
                (
                    vendor_dir.name,
                    soc_name,
                    vendor_dir / "op_impl" / "ai_core" / "tbe" / "config" / soc_name,
                )
            )
    return candidates


def collect_custom_optypes(
    ascend_home_path: Path,
    custom_opp_path: Optional[str],
    user_soc: str,
    soc_names: Sequence[str],
) -> Tuple[List[OpTypeSource], List[str]]:
    warnings = []
    sources = []
    vendors_root = ascend_home_path / "opp" / "vendors"
    if not vendors_root.exists():
        warnings.append(
            "${{ASCEND_HOME_PATH}}/opp/vendors not found: {}".format(vendors_root)
        )
    for vendor, matched_soc, path in _vendor_dirs(vendors_root, soc_names):
        if path.exists():
            sources.append(
                _scan_config_dir(CUSTOM, user_soc, matched_soc, path, vendor)
            )

    if not custom_opp_path:
        warnings.append(
            "ASCEND_CUSTOM_OPP_PATH is not set; "
            "only custom packages under ${ASCEND_HOME_PATH}/opp/vendors will be scanned."
        )
        return _deduplicate_sources(sources), warnings

    for custom_root_str in custom_opp_path.split(os.pathsep):
        if not custom_root_str:
            continue
        custom_root = Path(custom_root_str)
        if not custom_root.exists():
            warnings.append(
                "ASCEND_CUSTOM_OPP_PATH Path does not exist: {}".format(custom_root)
            )
        for vendor, matched_soc, path in _custom_opp_dirs(custom_root, soc_names):
            if path.exists():
                sources.append(
                    _scan_config_dir(CUSTOM, user_soc, matched_soc, path, vendor)
                )
    return _deduplicate_sources(sources), warnings


def _available_soc_dir_names(
    ascend_home_path: Path, custom_opp_path: Optional[str] = None
) -> List[str]:
    roots = [
        ascend_home_path
        / "opp"
        / "built-in"
        / "op_impl"
        / "ai_core"
        / "tbe"
        / "config",
        ascend_home_path / "opp" / "vendors",
    ]
    if custom_opp_path:
        roots.extend(Path(path) for path in custom_opp_path.split(os.pathsep) if path)
    socs = set()
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.is_dir() and path.name.lower().startswith("ascend"):
                socs.add(path.name)
    return sorted(socs)


def scan_optypes(
    user_soc: str, need_builtin: bool = True, need_custom: bool = True
) -> ScanResult:
    ascend_home = os.environ.get(ASCEND_HOME_ENV)
    custom_opp_path = os.environ.get(CUSTOM_OPP_ENV)
    ascend_home_path = Path(ascend_home) if ascend_home else None
    soc_name_map = load_soc_name_map(ascend_home_path)
    soc_names = expand_soc_aliases(user_soc, soc_name_map)
    result = ScanResult(
        user_soc=user_soc,
        soc_names=soc_names,
        ascend_home_path=ascend_home_path,
        custom_opp_path=custom_opp_path,
    )
    if not ascend_home_path:
        result.errors.append(CANN_ENV_ERROR)
        result.errors.append(CANN_ENV_SOURCE_HINT)
        return result
    if not (ascend_home_path / "opp").exists():
        result.errors.append(CANN_ENV_ERROR)
        result.errors.append(CANN_ENV_SOURCE_HINT)
        return result
    if not soc_names:
        result.errors.append(_format_unsupported_soc_error(result.user_soc))
        return result

    if need_builtin:
        result.builtin_sources = collect_builtin_optypes(
            ascend_home_path, result.user_soc, soc_names
        )
        if not result.builtin_sources:
            result.errors.append(_format_unsupported_soc_error(result.user_soc))
    if need_custom:
        result.custom_sources, custom_warnings = collect_custom_optypes(
            ascend_home_path, custom_opp_path, result.user_soc, soc_names
        )
        result.warnings.extend(custom_warnings)
        if not result.custom_sources:
            available_soc_dir_names = _available_soc_dir_names(
                ascend_home_path, custom_opp_path
            )
            if available_soc_dir_names and not set(soc_names).intersection(
                {soc.lower() for soc in available_soc_dir_names}
            ):
                result.errors.append(_format_unsupported_soc_error(result.user_soc))
            else:
                result.warnings.append(
                    "No custom packages found. Checked ${ASCEND_HOME_PATH}/opp/vendors "
                    "and ASCEND_CUSTOM_OPP_PATH."
                )
    return result


def _format_unsupported_soc_error(user_soc: str) -> str:
    return "Error: SoC version is not supported: {}".format(user_soc)


def detect_conflicts(
    builtin_sources: Sequence[OpTypeSource], custom_sources: Sequence[OpTypeSource]
) -> ConflictReport:
    report = ConflictReport()
    builtin_all = set()
    for source in builtin_sources:
        builtin_all.update(source.optypes)
    builtin_reference = builtin_sources[0] if builtin_sources else None
    if builtin_reference:
        for custom in custom_sources:
            conflicts = sorted(custom.optypes.intersection(builtin_all))
            if conflicts:
                report.custom_builtin.append(
                    ConflictGroup(
                        "Custom package conflicts with built-in OpTypes",
                        custom,
                        builtin_reference,
                        conflicts,
                    )
                )

    for index, left in enumerate(custom_sources):
        for right in custom_sources[index + 1 :]:
            if left.root_path == right.root_path:
                continue
            conflicts = sorted(left.optypes.intersection(right.optypes))
            if conflicts:
                report.custom_custom.append(
                    ConflictGroup(
                        "Custom package conflicts with another custom package",
                        left,
                        right,
                        conflicts,
                    )
                )
    return report


def _print_kv(label: str, value: Any, width: int = 22) -> None:
    print("  {label:<{width}} : {value}".format(label=label, width=width, value=value))


def _format_table(headers: Sequence[str], rows: Sequence[Sequence[Any]]) -> List[str]:
    str_rows = [[str(item) for item in row] for row in rows]
    widths = [len(header) for header in headers]
    for row in str_rows:
        for index, item in enumerate(row):
            widths[index] = max(widths[index], len(item))
    header_line = "  " + "  ".join(
        header.ljust(widths[index]) for index, header in enumerate(headers)
    )
    separator = "  " + "  ".join("-" * widths[index] for index in range(len(headers)))
    body = [
        "  " + "  ".join(item.ljust(widths[index]) for index, item in enumerate(row))
        for row in str_rows
    ]
    return [header_line, separator] + body


def _print_scan_info(result: ScanResult) -> None:
    print("[Scan Info]")
    _print_kv("SoC", result.user_soc)
    _print_kv("ASCEND_HOME_PATH", result.ascend_home_path or "<unset>")
    _print_kv("ASCEND_CUSTOM_OPP_PATH", result.custom_opp_path or "<unset>")
    print("")


def _all_sources(result: ScanResult) -> List[OpTypeSource]:
    return result.builtin_sources + result.custom_sources


def _print_sources(result: ScanResult) -> None:
    print("[Sources]")
    rows = [
        [
            source.source_type,
            source.status,
            len(source.optypes),
            len(source.config_files),
            source.soc,
            source.root_path,
        ]
        for source in _all_sources(result)
    ]
    if rows:
        for line in _format_table(
            ["Type", "Status", "OpTypes", "ConfigFiles", "SoC", "Path"], rows
        ):
            print(line)
    else:
        print("  <no matching sources>")
    print("")


def _print_messages(title: str, messages: Iterable[str]) -> None:
    messages = [msg for msg in messages if msg]
    if not messages:
        return
    print("[{}]".format(title))
    for msg in messages:
        for line in str(msg).splitlines():
            print("  {}".format(line))
    print("")


def _print_source_messages(result: ScanResult) -> None:
    warnings = list(result.warnings)
    errors = list(result.errors)
    for source in _all_sources(result):
        warnings.extend(source.warnings)
        errors.extend(source.errors)
    _print_messages("Warnings", warnings)
    _print_messages("Errors", errors)


def _print_list_summary(result: ScanResult, mode: str, count: int) -> None:
    print("[OpType List]")
    _print_kv("SoC", result.user_soc)
    _print_kv("Mode", mode)
    _print_kv("Total OpTypes", count)
    print("")


def _print_source_optypes(source: OpTypeSource) -> None:
    source_optypes = sorted(source.optypes)
    print("  [{type}] {path}".format(type=source.source_type, path=source.root_path))
    _print_kv("OpTypes", len(source_optypes), width=16)
    _print_kv("SoC", source.soc, width=16)
    if source_optypes:
        print("    OpType")
        print("    ------")
        for optype in source_optypes:
            print("    {}".format(optype))
    else:
        print("    <empty>")
    print("")


def _print_list(result: ScanResult, mode: str) -> None:
    if mode == BUILTIN:
        selected_sources = result.builtin_sources
    elif mode == CUSTOM:
        selected_sources = result.custom_sources
    else:
        selected_sources = _all_sources(result)
    optypes = sorted(
        {optype for source in selected_sources for optype in source.optypes}
    )
    _print_list_summary(result, mode, len(optypes))
    if len(selected_sources) > 1:
        for source in selected_sources:
            _print_source_optypes(source)
    else:
        if optypes:
            for line in _format_table(["OpType"], [[optype] for optype in optypes]):
                print(line)
        else:
            print("  <empty>")
        print("")


def _print_conflicts(result: ScanResult, report: ConflictReport) -> None:
    builtin_count = len(
        {optype for source in result.builtin_sources for optype in source.optypes}
    )
    print("[Conflict Summary]")
    _print_kv("SoC", result.user_soc)
    _print_kv("Built-in OpTypes", builtin_count)
    _print_kv("Custom packages", len(result.custom_sources))
    _print_kv("Custom vs Built-in", "{} group(s)".format(len(report.custom_builtin)))
    _print_kv("Custom vs Custom", "{} group(s)".format(len(report.custom_custom)))
    if not report.has_conflicts:
        _print_kv("Result", "No duplicate OpType conflicts found.")
    print("")

    conflict_index = 1
    for group in report.custom_builtin + report.custom_custom:
        print("[Conflict {}] {}".format(conflict_index, group.conflict_type))
        rows = [
            [
                "A",
                group.left.source_type,
                group.left.vendor_name or "-",
                group.left.root_path,
            ],
            [
                "B",
                group.right.source_type,
                group.right.vendor_name or "-",
                group.right.root_path,
            ],
        ]
        for line in _format_table(["Pkg", "Type", "Vendor", "Path"], rows):
            print(line)
        _print_kv("Conflict count", len(group.optypes))
        print("  Conflict OpTypes:")
        for optype in group.optypes:
            print("    - {}".format(optype))
        print("")
        conflict_index += 1


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="optype_collector",
        description="Collect OpTypes from CANN OPP built-in/custom packages and detect duplicate OpTypes.",
    )
    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument(
        "--builtin",
        action="store_true",
        help="List built-in OpTypes only. This is the default list mode.",
    )
    mode_group.add_argument(
        "--custom", action="store_true", help="List custom OpTypes only."
    )
    mode_group.add_argument(
        "--all", action="store_true", help="List all built-in and custom OpTypes."
    )
    parser.add_argument(
        "--detect-conflicts",
        metavar="SOC_VERSION",
        help="Detect duplicate OpTypes for the specified SoC.",
    )
    parser.add_argument(
        "soc_version", nargs="?", help="Specify the public SoC version."
    )
    return parser


def _list_mode(args: argparse.Namespace) -> str:
    if args.custom:
        return CUSTOM
    if args.all:
        return "all"
    return BUILTIN


def main(argv: Optional[Sequence[str]] = None) -> int:
    argv = list(argv) if argv is not None else sys.argv[1:]
    parser = _build_parser()
    args = parser.parse_args(argv)

    if args.detect_conflicts:
        result = scan_optypes(
            args.detect_conflicts, need_builtin=True, need_custom=True
        )
        _print_scan_info(result)
        _print_sources(result)
        report = detect_conflicts(result.builtin_sources, result.custom_sources)
        _print_conflicts(result, report)
        _print_source_messages(result)
        if result.errors:
            return 2
        return 1 if report.has_conflicts else 0

    if not args.soc_version:
        parser.print_help()
        return 2

    mode = _list_mode(args)
    result = scan_optypes(
        args.soc_version,
        need_builtin=mode in (BUILTIN, "all"),
        need_custom=mode in (CUSTOM, "all"),
    )
    _print_scan_info(result)
    _print_sources(result)
    _print_list(result, mode)
    _print_source_messages(result)
    return 2 if result.errors else 0


if __name__ == "__main__":
    sys.exit(main())
