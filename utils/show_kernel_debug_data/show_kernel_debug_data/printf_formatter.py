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

"""Format AscendC printf records stored as 8-byte FIFO argument slots."""

import ctypes
import re
import struct


_CONVERSION = re.compile(
    r"%(?P<flags>[-+ #0]*)(?P<width>\d+|\*)?"
    r"(?:\.(?P<precision>\d*|\*))?"
    r"(?P<length>hh|ll|h|l|j|z|t|L)?"
    r"(?P<specifier>[diuoxXfFeEgGaAcsp%])"
)
_INTEGER = "diuoxX"
_FLOAT = "fFeEgGaA"
_MAX_FIELD = 1_000_000
_LIBC = ctypes.CDLL(None)
_SNPRINTF = _LIBC.snprintf
_SNPRINTF.restype = ctypes.c_int


def _slot(buffer, offset, end):
    if offset + 8 > end:
        raise RuntimeError("printf argument exceeds FIFO argument area")
    return buffer[offset : offset + 8]


def _format_one(conversion, value):
    encoded = conversion.encode("ascii")
    size = _SNPRINTF(None, 0, encoded, value)
    if size < 0 or size > _MAX_FIELD:
        raise RuntimeError("printf conversion failed or output is too large")
    output = ctypes.create_string_buffer(size + 1)
    if _SNPRINTF(output, size + 1, encoded, value) != size:
        raise RuntimeError("printf conversion failed")
    return output.raw[:size].decode("utf-8", errors="replace")


def format_printf(fmt, buffer, args_start, args_end):
    """Render a FIFO printf payload using only validated C conversions."""
    pieces = []
    cursor = 0
    offset = args_start
    while cursor < len(fmt):
        marker = fmt.find("%", cursor)
        if marker < 0:
            pieces.append(fmt[cursor:])
            break
        pieces.append(fmt[cursor:marker])
        match = _CONVERSION.match(fmt, marker)
        if match is None:
            raise RuntimeError(
                f"unsupported printf conversion at offset {marker}: {fmt!r}"
            )
        flags, width, precision, length, specifier = match.group(
            "flags", "width", "precision", "length", "specifier"
        )
        length = length or ""
        if specifier == "%":
            if match.group(0) != "%%":
                raise RuntimeError("only %% is supported for a literal percent")
            pieces.append("%")
            cursor = match.end()
            continue

        if specifier in _INTEGER:
            if length == "L":
                raise RuntimeError("long double length is invalid for integer printf")
        elif specifier in _FLOAT:
            if length not in ("", "l"):
                raise RuntimeError("long double is not stored in the AscendC FIFO")
        elif length:
            raise RuntimeError(f"unsupported length modifier for %{specifier}")

        if width == "*":
            width = ctypes.c_int(
                struct.unpack("q", _slot(buffer, offset, args_end))[0]
            ).value
            offset += 8
            if width < 0:
                flags += "-"
                width = -width
        if precision == "*":
            precision = ctypes.c_int(
                struct.unpack("q", _slot(buffer, offset, args_end))[0]
            ).value
            offset += 8
            precision = None if precision < 0 else str(precision)
        if width is not None and int(width) > _MAX_FIELD:
            raise RuntimeError("printf width is too large")
        if precision is not None and int(precision or 0) > _MAX_FIELD:
            raise RuntimeError("printf precision is too large")

        slot = _slot(buffer, offset, args_end)
        if specifier in "di":
            raw = struct.unpack("q", slot)[0]
            if length in ("h", "hh"):
                bits = 16 if length == "h" else 8
                raw = raw & ((1 << bits) - 1)
                raw -= (1 << bits) if raw & (1 << (bits - 1)) else 0
            value = ctypes.c_longlong(raw)
            output_length = "ll"
        elif specifier in "uoxX":
            raw = struct.unpack("Q", slot)[0]
            if length in ("h", "hh"):
                raw &= 0xFFFF if length == "h" else 0xFF
            value = ctypes.c_ulonglong(raw)
            output_length = "ll"
        elif specifier in _FLOAT:
            if length == "l" or any(slot[4:]):
                raw = struct.unpack("d", slot)[0]
            else:
                raw = struct.unpack("f", slot[:4])[0]
            value = ctypes.c_double(raw)
            output_length = ""
        elif specifier == "s":
            relative = struct.unpack("Q", slot)[0]
            start = offset + relative
            if start >= len(buffer):
                raise RuntimeError("printf string offset exceeds FIFO record")
            end = buffer.find(b"\0", start)
            if end < 0:
                raise RuntimeError("printf string is not null terminated")
            value = ctypes.c_char_p(buffer[start:end])
            output_length = ""
        elif specifier == "p":
            value = ctypes.c_void_p(struct.unpack("Q", slot)[0])
            output_length = ""
        else:  # c
            value = ctypes.c_int(struct.unpack("q", slot)[0])
            output_length = ""
        offset += 8

        field = "" if width is None else str(width)
        if precision is not None:
            field += "." + str(precision)
        conversion = "%" + flags + field + output_length + specifier
        pieces.append(_format_one(conversion, value))
        cursor = match.end()

    return "".join(pieces)
