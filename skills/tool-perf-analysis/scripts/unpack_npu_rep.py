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
"""Unpack an NPU REP file into its original collection directory tree."""

import os
import shutil
import stat
import struct
import sys
from pathlib import Path


MAGIC = b"npu-rep\0"
VERSION = 0x00010000
ORIGIN = 1
ORIGIN_NAME = "profile"
HEADER_FORMAT = "<8sIHHIIIQ"
ENTRY_FORMAT = "<8s128sHHIQQ"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
ENTRY_SIZE = struct.calcsize(ENTRY_FORMAT)
NAME_SIZE = 128
NPU_REP_TYPE = 1
KNOWN_TYPES = {1, 2, 3, 4, 5, 6}
TYPE_NAMES = {
    1: "npu-rep",
    2: "json",
    3: "jsonl",
    4: "csv",
    5: "sqlite3",
    6: "protobuf",
}


class RepError(Exception):
    """Raised when a REP cannot be decoded safely."""


def fail(message):
    raise RepError(message)


def decode_name(raw_name):
    terminator = raw_name.find(b"\0")
    if terminator < 0:
        fail("rep file info name is not null terminated")
    name = os.fsdecode(raw_name[:terminator])
    if not name or name in (".", ".."):
        fail("rep file info name is unsafe")
    if "/" in name or "\\" in name:
        fail(f"rep file info name contains a path separator: {name}")
    return name


def decode_rep(encoded, logical_path):
    if len(encoded) < HEADER_SIZE:
        fail(f"invalid REP {logical_path}: rep is shorter than its header")
    magic, version, origin, head_size, count, entry_size, reserved, rep_size = (
        struct.unpack_from(HEADER_FORMAT, encoded, 0)
    )
    if magic != MAGIC:
        fail(f"invalid REP {logical_path}: invalid rep header magic")
    if version != VERSION:
        fail(f"invalid REP {logical_path}: unsupported rep version")
    if origin != ORIGIN:
        fail(f"invalid REP {logical_path}: unsupported rep origin")
    if head_size != HEADER_SIZE:
        fail(f"invalid REP {logical_path}: invalid rep header length")
    if entry_size != ENTRY_SIZE:
        fail(f"invalid REP {logical_path}: invalid rep file info length")
    if reserved != 0:
        fail(f"invalid REP {logical_path}: rep header reserved field is not zero")
    if rep_size != len(encoded):
        fail(f"invalid REP {logical_path}: rep length does not match input size")

    payload_start = head_size + count * entry_size
    if payload_start > len(encoded):
        fail(f"invalid REP {logical_path}: rep file info table exceeds input")

    entries = []
    names = set()
    expected_payload_offset = payload_start
    for index in range(count):
        offset = head_size + index * entry_size
        magic, raw_name, file_type, reserved16, reserved32, length, payload_offset = (
            struct.unpack_from(ENTRY_FORMAT, encoded, offset)
        )
        if magic != MAGIC:
            fail(f"invalid REP {logical_path}: invalid rep file info magic")
        name = decode_name(raw_name)
        if name in names:
            fail(
                f"invalid REP {logical_path}: rep contains duplicate file names: {name}"
            )
        names.add(name)
        if file_type not in KNOWN_TYPES:
            fail(f"invalid REP {logical_path}: rep entry has an unknown file type")
        if reserved16 != 0 or reserved32 != 0:
            fail(
                f"invalid REP {logical_path}: rep file info reserved field is not zero"
            )
        if (
            payload_offset != expected_payload_offset
            or payload_offset > len(encoded)
            or length > len(encoded) - payload_offset
        ):
            fail(f"invalid REP {logical_path}: invalid rep file payload range")
        payload = encoded[payload_offset : payload_offset + length]
        expected_payload_offset += length
        if file_type == NPU_REP_TYPE:
            _, child = decode_rep(payload, f"{logical_path}/{name}")
            entries.append((name, file_type, child))
        else:
            entries.append((name, file_type, payload))
    if expected_payload_offset != len(encoded):
        fail(f"invalid REP {logical_path}: rep contains unreferenced payload bytes")
    return (version, origin, count, rep_size), entries


def directory_name(entry_name):
    for suffix in (".npu.rep", ".rep"):
        if entry_name.endswith(suffix) and len(entry_name) > len(suffix):
            return entry_name[: -len(suffix)]
    fail(f"nested REP entry name must end with .npu.rep or .rep: {entry_name}")


def validate_tree(entries, logical_path):
    output_names = set()
    for name, file_type, payload in entries:
        output_name = directory_name(name) if file_type == NPU_REP_TYPE else name
        if output_name == ".hardware_info.lock":
            fail(
                f"HardwareInfo lock is not a collection result: {logical_path}/{output_name}"
            )
        if output_name in output_names:
            fail(
                f"imported entries conflict after unpacking: {logical_path}/{output_name}"
            )
        output_names.add(output_name)
        if file_type == NPU_REP_TYPE:
            validate_tree(payload, f"{logical_path}/{output_name}")


def write_tree(entries, output_directory):
    for name, file_type, payload in entries:
        output_name = directory_name(name) if file_type == NPU_REP_TYPE else name
        output_path = output_directory / output_name
        if file_type == NPU_REP_TYPE:
            output_path.mkdir(mode=0o700)
            write_tree(payload, output_path)
        else:
            with output_path.open("xb") as output:
                output.write(payload)


def print_usage():
    print("Usage: unpack_npu_rep.py <in.npu-rep> <out_dir>")


def parse_arguments():
    values = sys.argv[1:]
    if len(values) != 2:
        print_usage()
        return None
    return Path(values[0]), Path(values[1])


def version_text(version):
    return f"{version >> 16}.{(version >> 8) & 0xFF}.{version & 0xFF}"


def print_root_entries(header, entries):
    version, origin, count, rep_size = header
    if origin != ORIGIN:
        fail(f"unsupported rep origin: {origin}")
    print(
        f"[root] origin={ORIGIN_NAME} count={count} repLen={rep_size} ver={version_text(version)}"
    )
    for name, file_type, payload in entries:
        print(f"  - {name}  type={TYPE_NAMES[file_type]}  len={len(payload)}")


def main():
    arguments = parse_arguments()
    if arguments is None:
        return 1
    source = Path(os.path.abspath(arguments[0]))
    destination = Path(os.path.abspath(arguments[1]))
    try:
        source_status = source.lstat()
    except OSError as error:
        fail(f"inspect input REP failed: {source}: {error.strerror}")
    if not stat.S_ISREG(source_status.st_mode):
        fail(f"input REP is not a regular file: {source}")
    if destination.exists() or destination.is_symlink():
        fail(f"output directory already exists: {destination}")
    if not destination.parent.is_dir():
        fail(f"output directory parent is not a directory: {destination.parent}")

    header, entries = decode_rep(source.read_bytes(), str(source))
    validate_tree(entries, str(source))
    print_root_entries(header, entries)
    # mkdir 与 write_tree 分段处理：mkdir 抛 FileExistsError 说明目录并非本次创建
    # （TOCTOU 窗口内被外部创建），直接报错不删除；只有 mkdir 成功后 write 失败，
    # 才清理本次创建的目录（避免误删外部数据）
    try:
        destination.mkdir(mode=0o700)
    except FileExistsError:
        fail(f"output directory already exists: {destination}")
    except OSError as error:
        fail(f"create output directory failed: {destination}: {error}")
    try:
        write_tree(entries, destination)
    except OSError as error:
        shutil.rmtree(destination, ignore_errors=True)
        fail(f"write unpacked data failed: {error}")
    print(f"[OK] unpacked {source} -> {destination}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RepError as error:
        print(f"npu-rep-unpack: {error}", file=sys.stderr)
        sys.exit(1)
