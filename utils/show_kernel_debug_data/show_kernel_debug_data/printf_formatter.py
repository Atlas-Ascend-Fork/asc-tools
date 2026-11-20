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
    r"(?P<length>hh|ll|h|l|j|z|Z|t|L)?"
    r"(?P<specifier>[diuoxXfFeEgGaAcsp%])"
)
_INTEGER = "diuoxX"
_FLOAT = "fFeEgGaA"
_MAX_FIELD = 1_000_000
_ARG_SLOT_SIZE = struct.calcsize("Q")
_LIBC = ctypes.CDLL(None)
_SNPRINTF = _LIBC.snprintf
_SNPRINTF.restype = ctypes.c_int


def _slot(buffer, offset, end):
    if offset + _ARG_SLOT_SIZE > end:
        raise RuntimeError("printf argument exceeds FIFO argument area")
    return buffer[offset : offset + _ARG_SLOT_SIZE]


def _format_one(conversion, value):
    encoded = conversion.encode("ascii")
    size = _SNPRINTF(None, 0, encoded, value)
    if size < 0 or size > _MAX_FIELD:
        raise RuntimeError("printf conversion failed or output is too large")
    output = ctypes.create_string_buffer(size + 1)
    if _SNPRINTF(output, size + 1, encoded, value) != size:
        raise RuntimeError("printf conversion failed")
    return output.raw[:size].decode("utf-8", errors="replace")


def _validate_length(specifier, length):
    if specifier in _INTEGER:
        if length == "L":
            raise RuntimeError("long double length is invalid for integer printf")
    elif specifier in _FLOAT:
        if length not in ("", "l"):
            raise RuntimeError("long double is not stored in the AscendC FIFO")
    elif length:
        raise RuntimeError(f"unsupported length modifier for %{specifier}")


def _read_format_fields(fields, buffer, offset, args_end):
    flags, width, precision = fields
    if width == "*":
        width = ctypes.c_int(
            struct.unpack("q", _slot(buffer, offset, args_end))[0]
        ).value
        offset += _ARG_SLOT_SIZE
        if width < 0:
            flags += "-"
            width = -width
    if precision == "*":
        precision = ctypes.c_int(
            struct.unpack("q", _slot(buffer, offset, args_end))[0]
        ).value
        offset += _ARG_SLOT_SIZE
        precision = None if precision < 0 else str(precision)
    if width is not None and int(width) > _MAX_FIELD:
        raise RuntimeError("printf width is too large")
    if precision is not None and int(precision or 0) > _MAX_FIELD:
        raise RuntimeError("printf precision is too large")
    return flags, width, precision, offset


def _read_signed_integer(slot, length):
    raw = struct.unpack("q", slot)[0]
    short_bits = {"h": 16, "hh": 8}
    if length in short_bits:
        bits = short_bits[length]
        raw = raw & ((1 << bits) - 1)
        raw -= (1 << bits) if raw & (1 << (bits - 1)) else 0
    if length in ("", "h", "hh"):
        return ctypes.c_int(raw), ""
    return ctypes.c_longlong(raw), "ll"


def _read_unsigned_integer(slot, length):
    raw = struct.unpack("Q", slot)[0]
    short_masks = {"h": 0xFFFF, "hh": 0xFF}
    if length in short_masks:
        raw &= short_masks[length]
    if length in ("", "h", "hh"):
        return ctypes.c_uint(raw), ""
    return ctypes.c_ulonglong(raw), "ll"


def _read_string(buffer, offset, relative, record_end):
    start = offset + relative
    if relative < _ARG_SLOT_SIZE or start >= record_end:
        raise RuntimeError("printf string offset exceeds FIFO record")
    end = buffer.find(b"\0", start, record_end)
    if end < 0:
        raise RuntimeError("printf string is not null terminated within FIFO record")
    return ctypes.c_char_p(buffer[start:end])


def _read_printf_value(specifier, length, buffer, offset, record_bounds):
    args_end, record_end = record_bounds
    slot = _slot(buffer, offset, args_end)
    if specifier in "di":
        return _read_signed_integer(slot, length)
    if specifier in "uoxX":
        return _read_unsigned_integer(slot, length)
    if specifier in _FLOAT:
        float_size = struct.calcsize("f")
        if length == "l" or any(slot[float_size:]):
            raw = struct.unpack("d", slot)[0]
        else:
            raw = struct.unpack("f", slot[:float_size])[0]
        return ctypes.c_double(raw), ""
    if specifier == "s":
        relative = struct.unpack("Q", slot)[0]
        return _read_string(buffer, offset, relative, record_end), ""
    if specifier == "p":
        return ctypes.c_void_p(struct.unpack("Q", slot)[0]), ""
    # c
    return ctypes.c_int(struct.unpack("q", slot)[0]), ""


def format_printf(fmt, buffer, args_start, args_end, record_end=None):
    """Render a FIFO printf payload using only validated C conversions."""
    if record_end is None:
        record_end = len(buffer)
    if not (0 <= args_start <= args_end <= record_end <= len(buffer)):
        raise RuntimeError("invalid printf record bounds")
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
        if length == "Z":
            length = "z"
        if specifier == "%":
            if match.group(0) != "%%":
                raise RuntimeError("only %% is supported for a literal percent")
            pieces.append("%")
            cursor = match.end()
            continue
        _validate_length(specifier, length)
        flags, width, precision, offset = _read_format_fields(
            (flags, width, precision), buffer, offset, args_end
        )
        value, output_length = _read_printf_value(
            specifier, length, buffer, offset, (args_end, record_end)
        )
        offset += _ARG_SLOT_SIZE
        field = "" if width is None else str(width)
        if precision is not None:
            field += "." + str(precision)
        conversion = "%" + flags + field + output_length + specifier
        pieces.append(_format_one(conversion, value))
        cursor = match.end()
    return "".join(pieces)
