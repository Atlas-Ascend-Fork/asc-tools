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

"""
NPU .npu-rep 二进制打包脚本(支持递归层级)

布局:
  [NpuRepHead]
  [NpuRepFileInfo_1] ... [NpuRepFileInfo_n]
  [file1_data] ... [filen_data]

递归规则:
  - 打包某目录时,其下的子目录先递归打包成独立子 .npu.rep(内存字节),
    作为 type=npu-rep 的数据项,文件名 = 目录名 + ".npu.rep"。
  - 子 rep 内部又遵循同样布局,可无限嵌套。
  - 每个 rep 自含独立偏移空间,子 rep 内的偏移从其自身起始位置的 0 计起。
  - 根目录只作为打包输入,不作为数据项写入 rep。
  - .hardware_info.lock 不写入 rep; 其他普通文件按扩展名写入其所在目录对应的 rep。

结构体(小端, 无额外对齐填充):
  NpuRepHead (36 字节):
    magic[8]          = "npu-rep\\0"
    version           = 0x00010000 (1.0.0: major<<16 | minor<<8 | patch)
    origin            = uint16, 采集来源: profile=1
    repHeadLength     = uint16, Head 自身长度 (36)
    fileInfoCount     = uint32, 文件/子 rep 个数
    fileInfoLength    = uint32, 单个 FileInfo 长度 (160)
    reserved          = uint32, 保留 (0)
    npuRepLength      = uint64, 本 rep 整体长度
  NpuRepFileInfo (160 字节):
    magic[8]          = "npu-rep\\0"
    fileName[128]     = 文件名(仅 basename, 不足补\\0)
    type              = uint16, npu-rep=1 json=2 jsonl=3 csv=4 sqlite3=5 protobuf=6
    reserved          = uint16, 保留 (0)
    reserved1         = uint32, 保留 (0)
    fileLength        = uint64, 该项数据字节数
    fileRepOffset     = uint64, 该项数据在本 rep 中的绝对偏移
"""

import os
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
LOCK_NAME = ".hardware_info.lock"
FILE_TYPES = {
    ".json": 2,
    ".jsonl": 3,
    ".csv": 4,
    ".sqlite3": 5,
    ".pb": 6,
    ".protobuf": 6,
}


class RepError(Exception):
    """Raised when an input cannot be represented as an NPU REP."""


def fail(message):
    raise RepError(message)


def is_temporary_name(name):
    return ".tmp." in name or name.endswith(".tmp")


def validate_jsonl(path):
    line_count = 0
    last = b""
    with path.open("rb") as source:
        for line in source:
            last = line[-1:] if line else last
            if line.rstrip(b"\r\n"):
                line_count += 1
    if last != b"\n":
        fail(f"JSONL collection file does not end with a newline: {path}")
    if line_count < 5:
        fail(f"JSONL collection file contains fewer than five lines: {path}")


def validate_csv(path):
    with path.open("rb") as source:
        header = source.readline()
        if not header or not header.rstrip(b"\r\n"):
            fail(f"CSV collection file has no header: {path}")
        for line in source:
            if line.rstrip(b"\r\n"):
                return
    fail(f"CSV collection file has no data row: {path}")


def collection_file_type(path):
    suffix = path.suffix
    try:
        return FILE_TYPES[suffix]
    except KeyError:
        fail(f"unknown collection file type: {path}")


def read_collection_file(path):
    status = path.lstat()
    if not stat.S_ISREG(status.st_mode):
        fail(f"collection path is not a regular file: {path}")
    if status.st_size == 0:
        fail(f"collection file is empty: {path}")

    file_type = collection_file_type(path)
    if file_type == 3:
        validate_jsonl(path)
    elif file_type == 4:
        validate_csv(path)
    return file_type, path.read_bytes()


def validate_entry_name(name):
    encoded = os.fsencode(name)
    if not encoded:
        fail("rep entry file name is empty")
    if len(encoded) >= NAME_SIZE:
        fail(f"rep entry file name exceeds 127 bytes: {name}")
    if b"\0" in encoded:
        fail(f"rep entry file name contains a null byte: {name}")
    if b"/" in encoded or b"\\" in encoded:
        fail(f"rep entry file name contains a path separator: {name}")
    return encoded


def encode_rep(entries):
    names = set()
    payload_start = HEADER_SIZE + len(entries) * ENTRY_SIZE
    payload_length = sum(len(payload) for _, _, payload in entries)
    rep_length = payload_start + payload_length
    if rep_length > 0xFFFFFFFFFFFFFFFF:
        fail("rep length exceeds uint64_t")

    encoded = bytearray(rep_length)
    struct.pack_into(
        HEADER_FORMAT,
        encoded,
        0,
        MAGIC,
        VERSION,
        ORIGIN,
        HEADER_SIZE,
        len(entries),
        ENTRY_SIZE,
        0,
        rep_length,
    )

    payload_offset = payload_start
    for index, (name, file_type, payload) in enumerate(entries):
        name_bytes = validate_entry_name(name)
        if name_bytes in names:
            fail(f"rep contains duplicate file names: {name}")
        names.add(name_bytes)
        name_field = name_bytes + b"\0" * (NAME_SIZE - len(name_bytes))
        entry_offset = HEADER_SIZE + index * ENTRY_SIZE
        struct.pack_into(
            ENTRY_FORMAT,
            encoded,
            entry_offset,
            MAGIC,
            name_field,
            file_type,
            0,
            0,
            len(payload),
            payload_offset,
        )
        encoded[payload_offset : payload_offset + len(payload)] = payload
        payload_offset += len(payload)
    return bytes(encoded)


def collect_entries(directory):
    entries = []
    try:
        children = sorted(directory.iterdir(), key=lambda path: os.fsencode(path.name))
    except OSError as error:
        fail(f"open collection directory failed: {directory}: {error.strerror}")

    stored_names = set()
    for path in children:
        try:
            status = path.lstat()
        except OSError as error:
            fail(f"inspect collection directory item failed: {path}: {error.strerror}")
        name = path.name
        if stat.S_ISLNK(status.st_mode):
            fail(f"symbolic link is not allowed in collection directory: {path}")
        if name == LOCK_NAME:
            if not stat.S_ISREG(status.st_mode):
                fail(f"HardwareInfo lock path is not a regular file: {path}")
            continue
        if is_temporary_name(name):
            fail(f"temporary collection item remains: {path}")

        if stat.S_ISDIR(status.st_mode):
            stored_name = name + ".npu.rep"
            file_type = 1
            payload = encode_rep(collect_entries(path))
        elif stat.S_ISREG(status.st_mode):
            stored_name = name
            file_type, payload = read_collection_file(path)
        else:
            fail(f"unsupported collection directory item: {path}")

        if stored_name in stored_names:
            fail(f"collection stored name conflict: {stored_name}")
        stored_names.add(stored_name)
        entries.append((stored_name, file_type, payload))
    return sorted(entries, key=lambda entry: os.fsencode(entry[0]))


def print_usage():
    print("Usage: pack_rep.py <data_dir> <out.npu-rep> [origin]")
    print("  origin: profile (default: profile)")


def parse_arguments():
    values = sys.argv[1:]
    if len(values) not in (2, 3) or (len(values) == 3 and values[2] != ORIGIN_NAME):
        print_usage()
        return None
    return Path(values[0]), Path(values[1])


def main():
    arguments = parse_arguments()
    if arguments is None:
        return 1
    source = Path(os.path.abspath(arguments[0]))
    destination = Path(os.path.abspath(arguments[1]))
    try:
        source_status = source.lstat()
    except OSError as error:
        fail(f"inspect collection path failed: {source}: {error.strerror}")
    if not stat.S_ISDIR(source_status.st_mode):
        fail(f"collection path is not a directory: {source}")
    if destination.exists() or destination.is_symlink():
        fail(f"output REP already exists: {destination}")
    if destination.suffix != ".npu-rep":
        fail(f"output REP file name must end with .npu-rep: {destination}")
    if not destination.parent.is_dir():
        fail(f"output REP parent is not a directory: {destination.parent}")

    encoded = encode_rep(collect_entries(source))
    try:
        with destination.open("xb") as output:
            output.write(encoded)
    except OSError as error:
        fail(f"write output REP failed: {destination}: {error.strerror}")
    print(f"[OK] packed {source} -> {destination}")
    print(f"     total size: {len(encoded)} bytes")
    print(f"     origin: {ORIGIN_NAME}")
    print(f"     version: 1.0.0 (0x{VERSION:08x})")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RepError as error:
        print(f"npu-rep-pack: {error}", file=sys.stderr)
        sys.exit(1)
