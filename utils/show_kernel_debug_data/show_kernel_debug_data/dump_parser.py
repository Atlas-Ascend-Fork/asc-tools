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

import glob
import json
import os
import stat
import struct
import subprocess
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import List, Dict, Any
from .data_converter import decode_bfloat16
from .dump_logger import DUMP_PARSER_LOG

FILE_FLAG = os.O_WRONLY | os.O_CREAT
FILE_MODE_640 = stat.S_IRUSR | stat.S_IWUSR | stat.S_IRGRP

ONE_MEGA_BYTE = 1024 * 1024
FIFO_SUPER_TENSOR_VALUE_FMT = "<IIIIHHI8II4xQQI4x"
FIFO_SUPER_TENSOR_BODY_VALUE_FMT = "<IIQQI4x"
FIFO_SKIP_TYPE = 10


class TimeStampId(Enum):
    TIME_STAMP_WRAP_FIRST = 0x000
    TIME_STAMP_WRAP_MC2_CTX = auto()
    TIME_STAMP_WRAP_WK_SPACE = auto()
    TIME_STAMP_WRAP_INIT_DUMP = auto()
    TIME_STAMP_WRAP_FFTS_ADDR = auto()
    TIME_STAMP_WRAP_CLEAR_WK_SPAC = auto()

    TIME_STAMP_TPIPE = 0x030
    TIME_STAMP_BUFFER = auto()

    TIME_STAMP_MATMUL_SERVER = 0x060
    TIME_STAMP_MATMUL_SERVER_INIT = auto()
    TIME_STAMP_MATMUL_SERVER_OBJ = auto()
    TIME_STAMP_MATMUL_MATRIX_KFC = auto()
    TIME_STAMP_MATMUL_CLIENT_KFC = auto()
    TIME_STAMP_MATMUL_WAIT_EVE = auto()
    TIME_STAMP_MATMUL_OBJ = auto()

    TIME_STAMP_TILING_DATA = 0x090
    TIME_STAMP_TILING_DATA_STRUCT = auto()
    TIME_STAMP_TILING_DATA_MEMBER = auto()

    TIME_STAMP_MAX = 0xFFF


def get_enum_member_name(enum_type, value):
    for member in enum_type:
        if member.value == value:
            return member.name
    return value


@dataclass(repr=False)
class TLV:
    tag: int = 0
    length: int = 0
    value: bytes = None

    def read(self, f):
        tl_fmt = self.get_tl_format()
        tl_size = self.get_tl_size()
        self.tag, self.length = struct.unpack(tl_fmt, f.read(tl_size))
        self.value = f.read(self.length)

    def write_to(self, buffer, offset):
        tl_fmt = self.get_tl_format()
        struct.pack_into(tl_fmt, buffer, offset, self.tag, self.length)
        value_offset = offset + self.get_tl_size()
        val_fmt = f"{self.length}s"
        struct.pack_into(val_fmt, buffer, value_offset, self.value)
        return self.get_tl_size() + self.length

    def __repr__(self):
        return f"TLV(tag={self.tag}, length={self.length})"

    @classmethod
    def get_tl_format(cls):
        # TLV header uses uint32_t type/length.
        return "II"

    @classmethod
    def get_tl_size(cls):
        return struct.calcsize(cls.get_tl_format())

    def get_size(self):
        return self.get_tl_size() + self.length


@dataclass
class DumpMessageHeader:
    addr: int = 0
    data_type: int = 0
    desc: int = 0
    buffer_id: int = 0
    position: int = 0
    reserved: int = 0

    @classmethod
    def get_format(cls):
        return "iiiiii"

    @classmethod
    def get_size(cls):
        fmt = cls.get_format()
        return struct.calcsize(fmt)

    def unpack(self, buffer):
        fmt = self.get_format()
        (
            self.addr,
            self.data_type,
            self.desc,
            self.buffer_id,
            self.position,
            self.reserved,
        ) = struct.unpack(fmt, buffer)

    def pack(self):
        fmt = self.get_format()
        return struct.pack(
            fmt,
            self.addr,
            self.data_type,
            self.desc,
            self.buffer_id,
            self.position,
            self.reserved,
        )


@dataclass
class ShapeInfo:
    dim: int = 0
    shape: List[Any] = field(default_factory=list)
    rsv: int = 0
    total_ele_num = 0

    @classmethod
    def get_format(cls):
        # value format of ShapeInfo bin: dim, shape0, ..., shape7, rsv
        return "iiiiiiiiii"

    @classmethod
    def get_size(cls):
        fmt = cls.get_format()
        return struct.calcsize(fmt)

    def unpack(self, buffer):
        fmt = self.get_format()
        unpacked_data = struct.unpack(fmt, buffer)
        self.dim = unpacked_data[0]
        self.total_ele_num = 1
        for i in range(self.dim):
            self.shape.append(unpacked_data[i + 1])
            self.total_ele_num *= self.shape[-1]

    def parse_from(self, tlv: TLV):
        self.unpack(tlv.value)


@dataclass
class MetaInfo:
    blk_dim: int = 0
    core_type: int = 0
    task_ration: int = 0
    rsv: int = 0
    content: str = ""

    @classmethod
    def get_format(cls):
        return "HbbI"

    @classmethod
    def get_size(cls):
        fmt = cls.get_format()
        return struct.calcsize(fmt)

    def unpack(self, buffer):
        core_type_convert = {0: "MIX", 1: "AIC", 2: "VEC"}
        fmt = self.get_format()
        self.blk_dim, self.core_type, self.task_ration, self.rsv = struct.unpack(
            fmt, buffer
        )
        self.content = "[Meta Info] block num: {}, core type: {}, isMix: {}\n".format(
            self.blk_dim,
            core_type_convert.get(self.core_type, "Unknown"),
            bool(self.task_ration),
        )

    def parse_from(self, tlv: TLV):
        self.unpack(tlv.value)


@dataclass
class TimeStampInfo:
    desc_id: int = 0
    rsv: int = 0
    sys_cycle: int = 0
    pc_ptr: int = 0

    @classmethod
    def get_format(cls):
        # value format of TimeStampInfo bin:desc_id(u32), rsv(u32), sys_cycle(u64)
        return "IIQQ"

    @classmethod
    def get_size(cls):
        fmt = cls.get_format()
        return struct.calcsize(fmt)

    def unpack(self, buffer):
        fmt = self.get_format()
        unpacked_data = struct.unpack(fmt, buffer)
        self.desc_id = unpacked_data[0]
        self.rsv = unpacked_data[1]
        self.sys_cycle = unpacked_data[2]
        self.pc_ptr = unpacked_data[3]

    def parse_from(self, tlv: TLV):
        self.unpack(tlv.value)


@dataclass
class FifoTimeStampInfo:
    desc_id: int = 0
    block_idx: int = 0
    rsv: int = 0
    sys_cycle: int = 0
    pc_ptr: int = 0
    entry: int = 0
    resv_mem0: int = 0
    resv_mem1: int = 0

    @classmethod
    def get_format(cls):
        # value format of ringbuf TimeStampTlvInfo:
        return "IHHQQQII"

    @classmethod
    def get_size(cls):
        fmt = cls.get_format()
        return struct.calcsize(fmt)

    def unpack(self, buffer):
        fmt = self.get_format()
        unpacked_data = struct.unpack(fmt, buffer)
        self.desc_id = unpacked_data[0]
        self.block_idx = unpacked_data[1]
        self.rsv = unpacked_data[2]
        self.sys_cycle = unpacked_data[3]
        self.pc_ptr = unpacked_data[4]
        self.entry = unpacked_data[5]
        self.resv_mem0 = unpacked_data[6]
        self.resv_mem1 = unpacked_data[7]

    def parse_from(self, tlv: TLV):
        self.unpack(tlv.value)


dtype_to_fmt = {
    0: "f",  # DT_FLOAT
    1: "e",  # DT_FLOAT16
    2: "b",  # DT_INT8
    3: "i",  # DT_INT32
    4: "B",  # DT_UINT8
    6: "h",  # DT_INT16
    7: "H",  # DT_UINT16
    8: "I",  # DT_UINT32
    9: "q",  # DT_UINT64
    10: "Q",  # DT_UINT64
    27: "H",  # DT_BF16
    33: "",  # DT_MAX
}

dtype_to_data_type = {
    0: "float32",  # DT_FLOAT
    1: "float16",  # DT_FLOAT16
    2: "int8",  # DT_INT8
    3: "int32",  # DT_INT32
    4: "uint8",  # DT_UINT8
    6: "int16",  # DT_INT16
    7: "uint16",  # DT_UINT16
    8: "uint32",  # DT_UINT32
    9: "int64",  # DT_INT64
    10: "uint64",  # DT_UINT64
    27: "bfloat16",  # DT_BF16
    33: "",  # DT_MAX
}

special_data_type = {
    27: decode_bfloat16,  # 'bfloat16'
}


@dataclass(repr=False)
class DumpTensor:
    tag: int = 0
    length: int = 0
    dump_header: DumpMessageHeader = None
    dump_data: bytes = b""
    dump_value: List[Any] = field(default_factory=list)
    dump_shape: List[int] = field(default_factory=list)

    def parse_from(self, tlv):
        self.tag = tlv.tag
        self.length = tlv.length

        head_size = DumpMessageHeader.get_size()
        head_buffer = tlv.value[0:head_size]
        self.dump_header = DumpMessageHeader()
        self.dump_header.unpack(head_buffer)
        self.dump_data = tlv.value[head_size:]
        self._parse_dump_data()

    def parse_to(self):
        tlv = TLV()
        tlv.tag = self.tag
        tlv.length = self.length
        head_buffer = self.dump_header.pack()

        tlv.value = head_buffer
        tlv.value += self.dump_data
        return tlv

    def __repr__(self):
        return f"DumpTensor(tag={self.tag}, length={self.length}, dump_header={self.dump_header},\
            tensor_shape={self.dump_shape}, dump_data_size={len(self.dump_data)})"

    def _parse_dump_data(self):
        fmt = dtype_to_fmt.get(self.dump_header.data_type, "")
        if not fmt:
            DUMP_PARSER_LOG.debug(
                f"data type {self.dump_header.data_type} is not supported"
            )
            return
        if self.dump_header.data_type in special_data_type.keys():
            converter = special_data_type[self.dump_header.data_type]
            special_data = [
                converter(value[0]) for value in struct.iter_unpack(fmt, self.dump_data)
            ]
            self.dump_value.extend(special_data)
        else:
            self.dump_value.extend(
                value[0] for value in struct.iter_unpack(fmt, self.dump_data)
            )


@dataclass(repr=False)
class FifoDumpTensor(DumpTensor):
    def parse_from(self, tlv):
        self.tag = tlv.tag
        self.length = tlv.length

        head_size = struct.calcsize("IIIIHHI8III")
        if len(tlv.value) < head_size:
            raise RuntimeError("FifoDumpTensor: invalid TLV value length")

        unpacked = struct.unpack("IIIIHHI8III", tlv.value[:head_size])
        tensor_addr = unpacked[0]
        data_type = unpacked[1]
        desc = unpacked[2]
        buffer_id = unpacked[3]
        position = unpacked[4]
        # unpacked[5] is blockIdx, [6] is dim, [7:15] shape, [15] resv1
        dump_size = unpacked[16]

        self.dump_header = DumpMessageHeader(
            addr=tensor_addr,
            data_type=data_type,
            desc=desc,
            buffer_id=buffer_id,
            position=position,
            reserved=0,
        )

        data_start = head_size
        data_end = data_start + dump_size
        if data_end > len(tlv.value):
            raise RuntimeError("FifoDumpTensor: dump_size out of range")
        self.dump_data = tlv.value[data_start:data_end]
        self._parse_dump_data()


@dataclass(repr=False)
class FifoSuperDumpTensor(DumpTensor):
    tensor_length: int = 0
    tensor_offset: int = 0

    def parse_from(self, tlv):
        self.tag = tlv.tag
        self.length = tlv.length

        fmt = FIFO_SUPER_TENSOR_VALUE_FMT
        head_size = struct.calcsize(fmt)
        if len(tlv.value) < head_size:
            raise RuntimeError("FifoSuperDumpTensor: invalid TLV value length")

        unpacked = struct.unpack(fmt, tlv.value[:head_size])
        dim = unpacked[6]
        if dim > 8:
            raise RuntimeError(f"FifoSuperDumpTensor: invalid shape dim={dim}")
        self.dump_header = DumpMessageHeader(
            addr=unpacked[0],
            data_type=unpacked[1],
            desc=unpacked[2],
            buffer_id=unpacked[3],
            position=unpacked[4],
            reserved=0,
        )
        self.dump_shape = list(unpacked[7 : 7 + dim])
        self.tensor_length = unpacked[16]
        self.tensor_offset = unpacked[17]
        dump_size = unpacked[18]
        data_end = head_size + dump_size
        if data_end > len(tlv.value):
            raise RuntimeError("FifoSuperDumpTensor: dump_size out of range")
        self.dump_data = tlv.value[head_size:data_end]


@dataclass(repr=False)
class FifoSuperDumpTensorBody:
    tag: int = 0
    length: int = 0
    tensor_length: int = 0
    tensor_offset: int = 0
    dump_data: bytes = b""

    def parse_from(self, tlv):
        self.tag = tlv.tag
        self.length = tlv.length

        fmt = FIFO_SUPER_TENSOR_BODY_VALUE_FMT
        head_size = struct.calcsize(fmt)
        if len(tlv.value) < head_size:
            raise RuntimeError("FifoSuperDumpTensorBody: invalid TLV value length")

        _, _, self.tensor_length, self.tensor_offset, dump_size = struct.unpack(
            fmt, tlv.value[:head_size]
        )
        data_end = head_size + dump_size
        if data_end > len(tlv.value):
            raise RuntimeError("FifoSuperDumpTensorBody: dump_size out of range")
        self.dump_data = tlv.value[head_size:data_end]


@dataclass(repr=False)
class PrintStruct:
    tag: int = 0
    length: int = 0
    fmt: str = ""
    args: List[Any] = field(default_factory=list)
    content: str = None

    def parse_from(self, tlv):
        self.tag = tlv.tag
        self.length = tlv.length

        args_start, args_end = self._read_fmt(tlv.value)  # fmt
        self._read_args(tlv.value, args_start, args_end)  # args

        self.fmt = self.fmt.replace("%p", "0x%x")  # python not support %p
        self.content = self.fmt % tuple(self.args)

    def _read_fmt(self, buffer):
        self.fmt, fmt_offset = self._read_arg_str(buffer, 0)
        return (8, fmt_offset)

    def _all_fmt_placehold(self):
        import re

        pattern = r"%[a-zA-Z]{1,2}"
        matches = re.findall(pattern, self.fmt)
        fmt_placehold_list = []
        for x in matches:
            if not (len(x) == 3 and "%l" not in x.lower()):
                fmt_placehold_list.append(x)
        return fmt_placehold_list

    def _read_args(self, buffer, args_start, args_end):
        args_plds = self._all_fmt_placehold()
        args_num = len(args_plds)

        for i in range(0, args_num):
            offset = args_start + i * 8
            if offset + 8 > args_end:
                raise RuntimeError(
                    f'arg {i} at [{offset}:{offset + 8}] over arg end({args_end}): "{self.fmt}"'
                )
            pld = args_plds[i]
            self._read_arg(buffer, offset, pld)

    def _read_arg(self, buffer, offset, pld):
        if pld == "%d" or pld == "%i":
            arg = self._read_arg_long(buffer, offset)
        elif pld == "%ld":
            arg = self._read_arg_long(buffer, offset)
        elif pld == "%f" or pld == "%F":
            arg = self._read_arg_float(buffer, offset)
        elif pld == "%lf" or pld == "%LF":
            arg = self._read_arg_double(buffer, offset)
        elif pld == "%x" or pld == "%X":
            arg = self._read_arg_hex(buffer, offset)
        elif pld == "%s":
            arg, _ = self._read_arg_str(buffer, offset)
        elif pld == "%p":
            arg = self._read_arg_point(buffer, offset)
        elif pld == "%u":
            arg = self._read_arg_unsigned_long(buffer, offset)
        else:
            raise RuntimeError(f"not support pld({pld}), fmt: {self.fmt}")
        self.args.append(arg)

    @staticmethod
    def _read_arg_unsigned_long(buffer, offset):
        return struct.unpack("Q", buffer[offset : offset + 8])[0]

    @staticmethod
    def _read_arg_long(buffer, offset):
        return struct.unpack("q", buffer[offset : offset + 8])[0]

    @staticmethod
    def _read_arg_float(buffer, offset):
        is_double = False
        for buf_val in buffer[offset + 4 : offset + 8]:
            if int(buf_val) != 0:
                is_double = True
                break
        if is_double:
            return struct.unpack("d", buffer[offset : offset + 8])[0]
        return struct.unpack("f", buffer[offset : offset + 4])[0]

    @staticmethod
    def _read_arg_double(buffer, offset):
        return struct.unpack("d", buffer[offset : offset + 8])[0]

    @staticmethod
    def _read_arg_hex(buffer, offset):
        return struct.unpack("Q", buffer[offset : offset + 8])[0]

    @staticmethod
    def _read_arg_str(buffer, offset):
        relv_offset = struct.unpack("Q", buffer[offset : offset + 8])[0]
        abs_offset = offset + relv_offset
        return (PrintStruct._read_string(buffer, abs_offset), relv_offset)

    @staticmethod
    def _read_arg_point(buffer, offset):
        return struct.unpack("P", buffer[offset : offset + 8])[0]

    @staticmethod
    def _read_string(buffer: bytes, offset: int):
        max_length = len(buffer)
        if offset > max_length:
            raise RuntimeError(
                f"read str offset {offset} over max buffer length {max_length}"
            )
        s: str = ""
        for i in range(offset, max_length):
            b = struct.unpack("1s", buffer[i : i + 1])[0]
            if b == b"\x00":
                return s
            s = s + b.decode("utf-8")
        return s


@dataclass(repr=False)
class FifoPrintStruct(PrintStruct):
    block_idx: int = 0
    resv: int = 0

    def parse_from(self, tlv):
        self.tag = tlv.tag
        self.length = tlv.length

        if len(tlv.value) < 16:
            raise RuntimeError("FifoPrintStruct: invalid TLV value length")
        self.block_idx, self.resv, _fmt_offset = struct.unpack("IIQ", tlv.value[:16])

        # Skip blockIdx/resv (8 bytes) so buffer starts at fmtOffset field.
        fifo_buffer = tlv.value[8:]
        args_start, args_end = self._read_fmt(fifo_buffer)  # fmt
        self._read_args(fifo_buffer, args_start, args_end)  # args

        self.fmt = self.fmt.replace("%p", "0x%x")  # python not support %p
        self.content = self.fmt % tuple(self.args)


@dataclass(repr=False)
class FifoSimtPrintStruct(PrintStruct):
    block_idx: List[int] = field(default_factory=lambda: [0, 0, 0])
    thread_idx: List[int] = field(default_factory=lambda: [0, 0, 0])
    resv: List[int] = field(default_factory=lambda: [0, 0, 0, 0])

    def parse_from(self, tlv):
        self.tag = tlv.tag
        self.length = tlv.length

        header_size = struct.calcsize("3I3I4IQ")
        if len(tlv.value) < header_size:
            raise RuntimeError("FifoSimtPrintStruct: invalid TLV value length")

        unpacked = struct.unpack("3I3I4IQ", tlv.value[:header_size])
        self.block_idx = list(unpacked[0:3])
        self.thread_idx = list(unpacked[3:6])
        self.resv = list(unpacked[6:10])

        # Buffer starts from fmtOffset field.
        simt_buffer = tlv.value[40:]
        args_start, args_end = self._read_fmt(simt_buffer)  # fmt
        self._read_args(simt_buffer, args_start, args_end)  # args

        self.fmt = self.fmt.replace("%p", "0x%x")  # python not support %p
        self.content = self.fmt % tuple(self.args)


@dataclass
class BlockInfo:
    total_size: int = 0
    block_id: int = 0
    block_num: int = 0
    remain_size: int = 0
    magic_num: int = 0
    reserved: int = 0
    dump_addr: int = 0  # 8 Byte

    @classmethod
    def get_format(cls):
        return "iiiiiiQ"

    @classmethod
    def get_size(cls):
        fmt = cls.get_format()
        return struct.calcsize(fmt)

    def unpack(self, buffer):
        fmt = self.get_format()
        (
            self.total_size,
            self.block_id,
            self.block_num,
            self.remain_size,
            self.magic_num,
            self.reserved,
            self.dump_addr,
        ) = struct.unpack(fmt, buffer)

    def pack_into(self, buffer, offset):
        fmt = self.get_format()
        struct.pack_into(
            fmt,
            buffer,
            offset,
            self.total_size,
            self.block_id,
            self.block_num,
            self.remain_size,
            self.magic_num,
            self.reserved,
            self.dump_addr,
        )
        return self.get_size()

    def is_valid(self):
        block_info_magic = 0x5AA5BCCD
        return block_info_magic == self.magic_num


@dataclass
class FifoBlockInfo:
    length: int = 0
    core_id: int = 0
    block_num: int = 0
    remain_len: int = 0
    magic: int = 0
    flag: int = 0
    rsv: int = 0
    dump_addr: int = 0
    resv: List[int] = field(default_factory=list)

    def __repr__(self):
        return (
            f"FifoBlockInfo(length={self.length}, core_id={self.core_id}, block_num={self.block_num}, "
            f"remain_len={self.remain_len}, magic=0x{self.magic:04X}, flag={self.flag}, rsv={self.rsv}, "
            f"dump_addr=0x{self.dump_addr:X}, resv={self.resv})"
        )

    @classmethod
    def get_format(cls):
        return "IIIIHHIQ6I"

    @classmethod
    def get_size(cls):
        fmt = cls.get_format()
        return struct.calcsize(fmt)

    def unpack(self, buffer):
        fmt = self.get_format()
        unpacked = struct.unpack(fmt, buffer)
        self.length = unpacked[0]
        self.core_id = unpacked[1]
        self.block_num = unpacked[2]
        self.remain_len = unpacked[3]
        self.magic = unpacked[4]
        self.flag = unpacked[5]
        self.rsv = unpacked[6]
        self.dump_addr = unpacked[7]
        self.resv = list(unpacked[8:14])

    def is_valid(self):
        return self.magic == 0xAE86


@dataclass
class DumpCoreContent:
    block_info: BlockInfo = None
    dump_tensor_map: Dict[int, List[DumpTensor]] = field(default_factory=dict)
    print_list: List[str] = field(default_factory=list)
    simt_print_map: Dict[str, List[str]] = field(default_factory=dict)
    time_stamp_list: List[TimeStampInfo] = field(default_factory=list)
    index_dtype_dt: Dict[int, str] = field(default_factory=dict)
    shape: List[int] = field(default_factory=list)

    @staticmethod
    def _write_dump_tensor_data(dump_tensor, dump_data_path):
        with open(dump_data_path, "wb+") as f:
            f.write(dump_tensor.dump_data)

    @staticmethod
    def _write_dump_tensor_value(dump_tensor, dump_value_path):
        write_content = ""
        total_ele_num = 0
        if dump_tensor.dump_shape:
            total_ele_num = 1
            for ele in dump_tensor.dump_shape:
                total_ele_num = total_ele_num * ele
        if total_ele_num != 0:
            value_len = len(dump_tensor.dump_value)
            if total_ele_num > value_len:
                DUMP_PARSER_LOG.warning(
                    f"tensor shape {dump_tensor.dump_shape} needs {total_ele_num} elements but only "
                    f'{value_len} dumped, missing values will be shown as "-"'
                )
            elif total_ele_num < value_len:
                DUMP_PARSER_LOG.warning(
                    f"tensor shape {dump_tensor.dump_shape} needs {total_ele_num} elements but "
                    f"{value_len} dumped, extra dumped values will be ignored"
                )

            shape = dump_tensor.dump_shape.copy()
            write_content = "[" * len(shape)
            for i in range(len(shape) - 2, -1, -1):
                shape[i] *= shape[i + 1]
            for i in range(total_ele_num):
                cnt = 0
                for s in shape:
                    if (i + 1) % s == 0:
                        cnt += 1
                if i < value_len:
                    write_content += str(dump_tensor.dump_value[i])
                else:
                    write_content += "-"
                if cnt:
                    write_content += "]" * cnt
                    if i != total_ele_num - 1:
                        write_content += ",\n"
                        write_content += "[" * cnt
                elif i != total_ele_num - 1:
                    write_content += ","
        if not write_content:
            line_count = 0
            for value in dump_tensor.dump_value:
                write_content += str(value) + ","
                line_count += 1
                if line_count == 8:
                    write_content += "\n"
                    line_count = 0
        with open(dump_value_path, "w+") as f:
            f.write(write_content)

    @classmethod
    def _flow_name(self):
        return "legacy"

    @classmethod
    def _create_dump_tensor(self):
        return DumpTensor()

    @classmethod
    def _create_print_struct(self, _tlv_tag=None):
        return PrintStruct()

    @classmethod
    def _create_time_stamp_info(self):
        return TimeStampInfo()

    def add_tlv_data(self, tlv):
        if tlv.tag == DumpType.TENSOR_TYPE.value:
            dump_tensor = self._create_dump_tensor()
            dump_tensor.parse_from(tlv)
            index = dump_tensor.dump_header.desc
            data_type = dump_tensor.dump_header.data_type
            if self.shape:
                dump_tensor.dump_shape = self.shape.copy()
            self.shape.clear()
            self.index_dtype_dt[index] = dtype_to_data_type.get(data_type, "")
            self._add_dump_tensor(dump_tensor)
        elif tlv.tag in (
            DumpType.SCALAR_TYPE.value,
            DumpType.ASSERT_TYPE.value,
            DumpType.SIMT_PRINTF_TYPE.value,
            DumpType.SIMT_ASSERT_TYPE.value,
        ):
            print_struct = self._create_print_struct(tlv.tag)
            print_struct.parse_from(tlv)
            self.print_list.append(print_struct.content)
            if isinstance(print_struct, FifoSimtPrintStruct):
                thread_id = "_".join([str(x) for x in print_struct.thread_idx])
                if thread_id not in self.simt_print_map:
                    self.simt_print_map[thread_id] = []
                self.simt_print_map[thread_id].append(print_struct.content)
        elif tlv.tag == DumpType.SHAPE_TYPE.value:
            shape_info = ShapeInfo()
            shape_info.parse_from(tlv)
            self.shape = shape_info.shape
        elif tlv.tag == DumpType.TIME_STAMP.value:
            time_stamp_info = self._create_time_stamp_info()
            time_stamp_info.parse_from(tlv)
            self.time_stamp_list.append(time_stamp_info)
        elif tlv.tag == DumpType.META_TYPE.value:
            meta_info = MetaInfo()
            meta_info.parse_from(tlv)
            self.print_list.append(meta_info.content)
        else:
            DUMP_PARSER_LOG.error(f"Invalid dump Type: {tlv.tag}")

    def get_core_id(self):
        if self.block_info is None:
            return "unknown"
        if hasattr(self.block_info, "core_id"):
            return str(self.block_info.core_id)
        return str(self.block_info.block_id)

    def write_dump_tensor_data(self, dump_tensor, dump_data_path):
        self._write_dump_tensor_data(dump_tensor, dump_data_path)

    def write_dump_tensor_value(self, dump_tensor, dump_value_path):
        self._write_dump_tensor_value(dump_tensor, dump_value_path)

    def write_time_stamp(self, core_output_dir):
        self._write_time_stamp(core_output_dir)

    def write_to_dir(self, output_dir):
        core_id = self.get_core_id()
        core_output_dir = os.path.join(output_dir, str(core_id))
        os.makedirs(core_output_dir, exist_ok=True)
        DUMP_PARSER_LOG.info(
            f"write core {core_id} dump data to dir: {core_output_dir}"
        )
        if self.dump_tensor_map:
            self._write_dump_tensor_by_index(core_output_dir)
        if self.time_stamp_list:
            self._write_time_stamp(core_output_dir)

    def show_print(self):
        if len(self.print_list) > 0:
            core_id = self.get_core_id()
            print(f"================ block.{core_id} begin ==============")
            print("".join(self.print_list), end="", flush=True)
            print(f"================ block.{core_id} end ================")
            DUMP_PARSER_LOG.info(
                f"================ block.{core_id} begin =============="
            )
            DUMP_PARSER_LOG.info("".join(self.print_list))
            DUMP_PARSER_LOG.info(
                f"================ block.{core_id} end ================"
            )

    def _add_dump_tensor(self, dump_tensor):
        index = dump_tensor.dump_header.desc
        if index not in self.dump_tensor_map:
            self.dump_tensor_map[index] = []
        DUMP_PARSER_LOG.debug(
            f"Tensor[{index}][{len(self.dump_tensor_map[index])}] = {dump_tensor}"
        )
        self.dump_tensor_map[index].append(dump_tensor)

    def _write_dump_tensor_by_loop(self, index, index_output_dir):
        loop_cnt = len(self.dump_tensor_map[index])
        core_id = self.get_core_id()
        for loop in range(0, loop_cnt):
            dump_tensor = self.dump_tensor_map[index][loop]
            dump_file_name = f"core_{core_id}_index_{index}_loop_{loop}.bin"
            dump_file_path = os.path.join(index_output_dir, dump_file_name)
            self._write_dump_tensor_data(dump_tensor, dump_file_path)

            parsed_dump_file_name = f"core_{core_id}_index_{index}_loop_{loop}.txt"
            parsed_dump_file_path = os.path.join(
                index_output_dir, parsed_dump_file_name
            )
            self._write_dump_tensor_value(dump_tensor, parsed_dump_file_path)

    def _write_dump_tensor_by_index(self, core_output_dir):
        for index in self.dump_tensor_map.keys():
            index_output_dir = os.path.join(core_output_dir, f"index_{index}")
            os.makedirs(index_output_dir, exist_ok=True)
            DUMP_PARSER_LOG.info(
                f"write index {index} tensor to dir: {index_output_dir}"
            )
            self._write_dump_tensor_by_loop(index, index_output_dir)

    def _write_time_stamp(self, core_output_dir):
        import csv

        os.makedirs(core_output_dir, exist_ok=True)
        DUMP_PARSER_LOG.info(f"write time_stamp data to dir: {core_output_dir}")
        core_id = self.get_core_id()
        dump_file_name = f"time_stamp_core_{core_id}.csv"
        parsed_dump_file_path = os.path.join(core_output_dir, dump_file_name)
        with open(parsed_dump_file_path, "w", encoding="utf-8-sig", newline="") as f:
            csv_write = csv.writer(f)
            csv_write.writerow(["打点标识", "Cycle", "Cycle间隔", "PC指针"])
            last_cycle = 0
            for time_stamp in self.time_stamp_list:
                csv_write.writerow(
                    [
                        str(get_enum_member_name(TimeStampId, int(time_stamp.desc_id))),
                        str(time_stamp.sys_cycle),
                        int(time_stamp.sys_cycle) - last_cycle,
                        int(time_stamp.pc_ptr),
                    ]
                )
                last_cycle = int(time_stamp.sys_cycle)


@dataclass
class FifoDumpCoreContent(DumpCoreContent):
    super_tensor: FifoSuperDumpTensor = None
    super_tensor_chunks: List[bytes] = field(default_factory=list)
    last_super_tensor: FifoSuperDumpTensor = None

    def _flow_name(self):
        return "fifo"

    def _create_dump_tensor(self):
        return FifoDumpTensor()

    def _create_print_struct(self, tlv_tag=None):
        if tlv_tag in (
            DumpType.SIMT_PRINTF_TYPE.value,
            DumpType.SIMT_ASSERT_TYPE.value,
        ):
            return FifoSimtPrintStruct()
        return FifoPrintStruct()

    def _create_time_stamp_info(self):
        return FifoTimeStampInfo()

    def _discard_super_tensor(self, message):
        self.super_tensor = None
        self.super_tensor_chunks.clear()
        raise RuntimeError(message)

    def _finish_super_tensor(self):
        dump_tensor = self.super_tensor
        dump_tensor.dump_data = b"".join(self.super_tensor_chunks)
        dump_tensor.dump_value.clear()
        dump_tensor._parse_dump_data()
        data_type = dump_tensor.dump_header.data_type
        self.index_dtype_dt[dump_tensor.dump_header.desc] = dtype_to_data_type.get(
            data_type, ""
        )
        self._add_dump_tensor(dump_tensor)
        self.last_super_tensor = dump_tensor
        self.super_tensor = None
        self.super_tensor_chunks.clear()

    def _start_super_tensor(self, tlv):
        if self.super_tensor is not None:
            duplicate = FifoSuperDumpTensor()
            duplicate.parse_from(tlv)
            if (
                duplicate.tensor_length == self.super_tensor.tensor_length
                and duplicate.tensor_offset == 0
                and duplicate.dump_header == self.super_tensor.dump_header
                and duplicate.dump_shape == self.super_tensor.dump_shape
                and duplicate.dump_data == self.super_tensor_chunks[0]
            ):
                return
            self._discard_super_tensor(
                "FifoDumpCoreContent: new super tensor before previous tensor completed"
            )
        dump_tensor = FifoSuperDumpTensor()
        dump_tensor.parse_from(tlv)
        if dump_tensor.tensor_offset != 0:
            self._discard_super_tensor(
                f"FifoDumpCoreContent: invalid super tensor offset={dump_tensor.tensor_offset}"
            )
        dump_size = len(dump_tensor.dump_data)
        if dump_size == 0 or dump_size > dump_tensor.tensor_length:
            self._discard_super_tensor(
                "FifoDumpCoreContent: invalid super tensor first chunk size"
            )
        self.super_tensor = dump_tensor
        self.super_tensor_chunks.append(dump_tensor.dump_data)
        if dump_size == dump_tensor.tensor_length:
            self._finish_super_tensor()

    def _append_super_tensor_body(self, tlv):
        if self.super_tensor is None:
            body = FifoSuperDumpTensorBody()
            body.parse_from(tlv)
            body_end = body.tensor_offset + len(body.dump_data)
            if (
                self.last_super_tensor is not None
                and body.tensor_length == len(self.last_super_tensor.dump_data)
                and body_end <= body.tensor_length
                and self.last_super_tensor.dump_data[body.tensor_offset : body_end]
                == body.dump_data
            ):
                return
            self._discard_super_tensor(
                "FifoDumpCoreContent: super tensor body without head"
            )
        body = FifoSuperDumpTensorBody()
        body.parse_from(tlv)
        expected_offset = sum(len(chunk) for chunk in self.super_tensor_chunks)
        if body.tensor_length != self.super_tensor.tensor_length:
            self._discard_super_tensor(
                "FifoDumpCoreContent: super tensor length changed"
            )
        if body.tensor_offset != expected_offset:
            assembled = b"".join(self.super_tensor_chunks)
            body_end = body.tensor_offset + len(body.dump_data)
            if (
                body_end <= expected_offset
                and assembled[body.tensor_offset : body_end] == body.dump_data
            ):
                return
            self._discard_super_tensor(
                f"FifoDumpCoreContent: non-contiguous super tensor offset={body.tensor_offset}, "
                f"expected={expected_offset}"
            )
        dump_size = len(body.dump_data)
        remaining = body.tensor_length - body.tensor_offset
        if dump_size == 0 or dump_size > remaining:
            self._discard_super_tensor(
                "FifoDumpCoreContent: super tensor body exceeds tensor length"
            )
        self.super_tensor_chunks.append(body.dump_data)
        if dump_size == remaining:
            self._finish_super_tensor()

    def add_tlv_data(self, tlv):
        if tlv.tag == DumpType.SUPER_TENSOR_TYPE.value:
            self._start_super_tensor(tlv)
            return
        if tlv.tag == DumpType.SUPER_TENSOR_BODY_TYPE.value:
            self._append_super_tensor_body(tlv)
            return
        if self.super_tensor is not None:
            self._discard_super_tensor(
                f"FifoDumpCoreContent: unexpected TLV type {tlv.tag} during super tensor assembly"
            )
        super().add_tlv_data(tlv)

    def ensure_super_tensor_complete(self):
        if self.super_tensor is not None:
            self._discard_super_tensor(
                "FifoDumpCoreContent: incomplete super tensor at end of file"
            )


class DumpType(Enum):
    DEFAULT_TYPE = 0
    SCALAR_TYPE = 1
    TENSOR_TYPE = 2
    SHAPE_TYPE = 3
    ASSERT_TYPE = 4
    META_TYPE = 5
    TIME_STAMP = 6
    SUPER_TENSOR_TYPE = 11
    SUPER_TENSOR_BODY_TYPE = 12
    SIMT_PRINTF_TYPE = 0xF0E00F0E
    SIMT_ASSERT_TYPE = 0xF0F00F0F


class DumpBinFile:
    def __init__(self, dump_bin):
        self.dump_bin = self._pre_process(dump_bin)
        self.dump_core_contents = []  # 每个core dump 内容
        self.index_dtype_dt = {}

    def _pre_process(self, dump_bin: str):
        dump_dir = os.path.dirname(dump_bin)
        temp_dir = os.path.join(dump_dir, "predump")
        dump_file_name = os.path.basename(dump_bin)
        install_path = get_install_path()
        search_re = f"{install_path}/**/operator_cmp/compare/msaccucmp.py"
        search_result = glob.glob(search_re, recursive=True)
        if not search_result or not os.path.exists(search_result[0]):
            return dump_bin
        msaccucmp_file = os.path.realpath(search_result[0])
        cmd = f'python3 "{msaccucmp_file}" convert -d "{dump_bin}" -t bin -out "{temp_dir}"'
        log_file_tmp = DUMP_PARSER_LOG.get_log_file()
        with open(log_file_tmp, "a+") as f:
            try:
                subprocess.run(
                    cmd,
                    stdout=f,
                    stderr=subprocess.STDOUT,
                    shell=True,
                    encoding="utf-8",
                    timeout=120,
                )
            except subprocess.TimeoutExpired:
                DUMP_PARSER_LOG.error(f"Command {cmd} TIME OUT.")
        dump_result_re = os.path.join(temp_dir, f"{dump_file_name}.space.*.bin")
        dump_result = glob.glob(dump_result_re, recursive=True)
        if dump_result and os.path.exists(dump_result[0]):
            DUMP_PARSER_LOG.info(f"Find new dump_bin use {dump_result[0]}")
            return dump_result[0]
        return dump_bin

    def parse(self):
        file_size = os.path.getsize(self.dump_bin)
        with open(self.dump_bin, "rb") as bin_file:
            self.get_dump_core_contents(bin_file, file_size)

    def get_dump_core_contents(self, bin_file, file_size):
        block_id = 0
        read_pos = 0
        total_block_size = ONE_MEGA_BYTE
        block_info_size = BlockInfo.get_size()

        while read_pos + block_info_size < file_size:
            DUMP_PARSER_LOG.debug(f"block.{block_id} read from: {read_pos}")
            core_content = DumpCoreContent()

            # read block info
            block_info_buffer = bin_file.read(block_info_size)
            block_info = BlockInfo()
            block_info.unpack(block_info_buffer)

            if block_info.reserved == 7:
                DUMP_PARSER_LOG.warning(
                    f"block.{block_id} remain space is NOT enough for last dump !!!"
                )

            if not block_info.is_valid():
                DUMP_PARSER_LOG.debug(
                    f"block.{block_id} block info is not valid, skip this block..."
                )
                read_pos += total_block_size
                bin_file.seek(read_pos)
                block_id += 1
                continue

            DUMP_PARSER_LOG.debug(block_info)
            core_content.block_info = block_info
            total_block_size = block_info.total_size

            # read tlv data
            tlv_offset = 0
            core_dump_size = (
                block_info.total_size - block_info_size - block_info.remain_size
            )
            while tlv_offset < core_dump_size:
                tlv = TLV()
                tlv.read(bin_file)
                core_content.add_tlv_data(tlv)
                tlv_offset += tlv.get_size()
                self.index_dtype_dt.update(core_content.index_dtype_dt)
            self.dump_core_contents.append(core_content)

            # read remain data
            read_pos += block_info.total_size
            bin_file.seek(read_pos)
            block_id += 1

    def write_result(self, output_dir):
        if len(self.dump_core_contents) == 0:
            DUMP_PARSER_LOG.debug("no dump data, exit...")
            return ""

        parse_output_dir = os.path.join(output_dir, "dump_data")
        DUMP_PARSER_LOG.info(f"write dump workspace result: {parse_output_dir}")
        os.makedirs(parse_output_dir, exist_ok=True)
        for core_content in self.dump_core_contents:
            core_content.write_to_dir(parse_output_dir)
        return parse_output_dir

    def write_index_dtype(self, output_dir):
        if len(self.index_dtype_dt) == 0:
            DUMP_PARSER_LOG.debug("no dump index, exit...")
            # remove index_dtype.json
            return
        json_path = os.path.join(output_dir, "dump_data", "index_dtype.json")
        with os.fdopen(os.open(json_path, FILE_FLAG, FILE_MODE_640), "w") as f:
            json.dump(self.index_dtype_dt, f, indent=4)

    def show_print(self):
        for core_content in self.dump_core_contents:
            core_content.show_print()


def get_install_path() -> str:
    ascend_home = os.environ.get("ASCEND_HOME_PATH")
    if not ascend_home:
        raise RuntimeError(
            "get install path env failed, Please set environment variables"
        )
    return ascend_home


class FifoDumpBinFile:
    def __init__(self, dump_bin, core_type, core_id):
        self.dump_bin = dump_bin
        self.core_type = core_type
        self.core_id = core_id
        self.dump_core_contents = []
        self.index_dtype_dt = {}

    @staticmethod
    def _iter_core_tensors(core_content):
        for index, tensors in core_content.dump_tensor_map.items():
            for loop, dump_tensor in enumerate(tensors):
                yield index, loop, dump_tensor

    @staticmethod
    def _skip_replayed_super_tensor_head_tail(bin_file, file_size, core_content):
        if core_content.super_tensor is None:
            return False
        tl_size = TLV.get_tl_size()
        replayed_tail_len = struct.calcsize(
            FIFO_SUPER_TENSOR_VALUE_FMT
        ) - struct.calcsize(FIFO_SUPER_TENSOR_BODY_VALUE_FMT)
        original_pos = bin_file.tell()
        candidate_pos = original_pos - tl_size + replayed_tail_len
        if candidate_pos + tl_size > file_size:
            return False
        bin_file.seek(candidate_pos)
        candidate_head = bin_file.read(tl_size)
        candidate_tag, candidate_length = struct.unpack(
            TLV.get_tl_format(), candidate_head
        )
        candidate_remain = file_size - bin_file.tell()
        ring_len = core_content.block_info.remain_len
        candidate_end = candidate_pos + tl_size + candidate_length
        stream_len = candidate_end - FifoBlockInfo.get_size() - replayed_tail_len
        ends_at_ring_boundary = (
            ring_len > 0 and stream_len >= 0 and stream_len % ring_len == 0
        )
        if (
            candidate_tag != FIFO_SKIP_TYPE
            or candidate_length > candidate_remain
            or not ends_at_ring_boundary
        ):
            bin_file.seek(original_pos)
            return False
        bin_file.seek(candidate_pos)
        DUMP_PARSER_LOG.debug(
            f"ignore {replayed_tail_len} bytes of replayed super tensor head tail"
        )
        return True

    def parse(self):
        file_size = os.path.getsize(self.dump_bin)
        core_content = FifoDumpCoreContent()

        with open(self.dump_bin, "rb") as bin_file:
            block_info_size = FifoBlockInfo.get_size()
            block_info_buffer = bin_file.read(block_info_size)
            if len(block_info_buffer) < block_info_size:
                raise RuntimeError("FifoDumpBinFile: incomplete BlockInfo header")
            block_info = FifoBlockInfo()
            block_info.unpack(block_info_buffer)
            if not block_info.is_valid():
                raise RuntimeError("FifoDumpBinFile: invalid BlockInfo magic")
            core_content.block_info = block_info
            while True:
                tl_head = bin_file.read(TLV.get_tl_size())
                if not tl_head:
                    break
                if len(tl_head) < TLV.get_tl_size():
                    raise RuntimeError("FifoDumpBinFile: incomplete TLV header")
                tlv = TLV()
                tlv.tag, tlv.length = struct.unpack(tlv.get_tl_format(), tl_head)
                remain_size = file_size - bin_file.tell()
                if tlv.length > remain_size:
                    if self._skip_replayed_super_tensor_head_tail(
                        bin_file, file_size, core_content
                    ):
                        continue
                    if (
                        core_content.super_tensor is None
                        and block_info.remain_len > 0
                        and remain_size <= block_info.remain_len
                    ):
                        DUMP_PARSER_LOG.debug(
                            f"ignore {remain_size + TLV.get_tl_size()} bytes of unused FIFO tail"
                        )
                        break
                    raise RuntimeError(
                        f"FifoDumpBinFile: TLV length overflow, length={tlv.length}, remain={remain_size}"
                    )
                tlv.value = bin_file.read(tlv.length)
                if len(tlv.value) < tlv.length:
                    raise RuntimeError("FifoDumpBinFile: incomplete TLV value")

                if tlv.tag in (
                    DumpType.TENSOR_TYPE.value,
                    DumpType.SUPER_TENSOR_TYPE.value,
                    DumpType.SUPER_TENSOR_BODY_TYPE.value,
                    DumpType.SCALAR_TYPE.value,
                    DumpType.ASSERT_TYPE.value,
                    DumpType.SIMT_PRINTF_TYPE.value,
                    DumpType.SIMT_ASSERT_TYPE.value,
                    DumpType.TIME_STAMP.value,
                    DumpType.SHAPE_TYPE.value,
                ):
                    core_content.add_tlv_data(tlv)
                    self.index_dtype_dt.update(core_content.index_dtype_dt)
            core_content.ensure_super_tensor_complete()
        self.dump_core_contents.append(core_content)

    def write_result(self, output_dir):
        if len(self.dump_core_contents) == 0:
            DUMP_PARSER_LOG.debug("no dump data, exit...")
            return ""

        parse_output_dir = os.path.join(output_dir, "dump_data")
        DUMP_PARSER_LOG.info(f"write dump workspace result: {parse_output_dir}")
        os.makedirs(parse_output_dir, exist_ok=True)
        for core_content in self.dump_core_contents:
            core_output_dir = os.path.join(
                parse_output_dir, str(core_content.get_core_id())
            )
            os.makedirs(core_output_dir, exist_ok=True)
            for index, loop, dump_tensor in self._iter_core_tensors(core_content):
                name_prefix = f"asc_kernel_data_{self.core_type}_{self.core_id}_index_{index}_loop_{loop}"
                dump_file_path = os.path.join(core_output_dir, f"{name_prefix}.bin")
                core_content.write_dump_tensor_data(dump_tensor, dump_file_path)

                parsed_dump_file_path = os.path.join(
                    core_output_dir, f"{name_prefix}.txt"
                )
                core_content.write_dump_tensor_value(dump_tensor, parsed_dump_file_path)
            if self.core_type == "simt" and core_content.simt_print_map:
                self._write_simt_print_by_thread(core_content, core_output_dir)
            if core_content.time_stamp_list:
                core_content.write_time_stamp(core_output_dir)
        return parse_output_dir

    def write_index_dtype(self, output_dir):
        if len(self.index_dtype_dt) == 0:
            DUMP_PARSER_LOG.debug("no dump index, exit...")
            return
        json_path = os.path.join(output_dir, "dump_data", "index_dtype.json")
        with os.fdopen(os.open(json_path, FILE_FLAG, FILE_MODE_640), "w") as f:
            json.dump(self.index_dtype_dt, f, indent=4)

    def show_print(self):
        for core_content in self.dump_core_contents:
            core_content.show_print()

    def _write_simt_print_by_thread(self, core_content, parse_output_dir):
        for thread_id, print_list in core_content.simt_print_map.items():
            name_prefix = (
                f"asc_kernel_data_{self.core_type}_{self.core_id}_thread_{thread_id}"
            )
            parsed_dump_file_path = os.path.join(parse_output_dir, f"{name_prefix}.txt")
            with open(parsed_dump_file_path, "w+") as f:
                f.write("".join(print_list))


def _make_parser_output_dir(output_path):
    from datetime import datetime, timezone

    output_dir = os.path.abspath(output_path)
    cur_time_str = datetime.now(tz=timezone.utc).strftime("%Y%m%d%H%M%S%f")
    output_dir = os.path.join(output_dir, f"PARSER_{cur_time_str}")
    os.makedirs(output_dir, exist_ok=True)
    return output_dir


def _core_type_from_fifo_flag(flag):
    flag_to_core_type = {0: "aic", 1: "aiv", 2: "simt"}
    return flag_to_core_type.get(flag, "fifo")


def _read_block_magic_core_id_and_flag(dump_bin):
    raw_magic = None
    fifo_core_id = None
    fifo_flag = None
    with open(dump_bin, "rb") as bin_file:
        header = bin_file.read(FifoBlockInfo.get_size())
        if len(header) >= 20:
            raw_magic = struct.unpack("I", header[16:20])[0]
            unpacked = struct.unpack(FifoBlockInfo.get_format(), header)
            fifo_core_id = unpacked[1]
            fifo_flag = unpacked[5]
    return raw_magic, fifo_core_id, fifo_flag


def _collect_bin_files(input_path):
    input_path = os.path.abspath(input_path)
    if os.path.isfile(input_path):
        return [input_path]
    if os.path.isdir(input_path):
        search_re = os.path.join(glob.escape(input_path), "**", "*.bin")
        return sorted(
            [
                bin_file
                for bin_file in glob.glob(search_re, recursive=True)
                if os.path.isfile(bin_file)
            ]
        )
    return []


def parse_dump_bin(bin_file_path, output_path, parse_output_dir=None, init_logger=True):
    dump_bin = os.path.abspath(bin_file_path)
    output_dir = (
        parse_output_dir if parse_output_dir else _make_parser_output_dir(output_path)
    )
    output_dir = os.path.abspath(output_dir)
    os.makedirs(output_dir, exist_ok=True)
    if init_logger:
        DUMP_PARSER_LOG.set_log_file(os.path.join(output_dir, "parser.log"))
        DUMP_PARSER_LOG.set_log_level(os.environ.get("ASCEND_GLOBAL_LOG_LEVEL", "3"))
    try:
        name_no_ext, ext = os.path.splitext(os.path.basename(dump_bin))
        parts = name_no_ext.split("_")
        is_asc_kernel_data = (
            ext == ".bin"
            and len(parts) == 5
            and parts[0] == "asc"
            and parts[1] == "kernel"
            and parts[2] == "data"
        )
        raw_magic, fifo_core_id, fifo_flag = _read_block_magic_core_id_and_flag(
            dump_bin
        )

        if raw_magic is not None and (raw_magic & 0xFFFF) == 0xAE86:
            if is_asc_kernel_data:
                core_type = parts[3]
                core_id = parts[4]
            else:
                core_type = _core_type_from_fifo_flag(fifo_flag)
                core_id = str(fifo_core_id) if fifo_core_id is not None else "unknown"
            dump_file = FifoDumpBinFile(dump_bin, core_type, core_id)
        elif raw_magic is None or raw_magic == 0x5AA5BCCD:
            if os.path.getsize(dump_bin) < BlockInfo.get_size():
                raise RuntimeError(
                    f"file too small ({os.path.getsize(dump_bin)} bytes), "
                    f"at least {BlockInfo.get_size()} bytes required for a valid workspace dump"
                )
            dump_file = DumpBinFile(dump_bin)
        else:
            raise RuntimeError(f"unknown block magic: 0x{raw_magic:08X}")

        dump_file.parse()
        dump_file.write_result(output_dir)
        dump_file.write_index_dtype(output_dir)
        dump_file.show_print()
        return 0
    except Exception:
        print(f"parse dump workspace bin occur exception, bin_file: {dump_bin}")
        import traceback

        traceback.print_exc()
        return 255


def _validate_and_prepare(bin_file_path, output_path):
    """Validate input/output paths and collect bin files.
    Returns (dump_bins, output_path, parser_output_dir) on success.
    parser_output_dir is None when bin_file_path points to a single file.
    """
    if not bin_file_path or not os.path.exists(bin_file_path):
        if bin_file_path and not all(ord(c) < 128 for c in bin_file_path):
            raise RuntimeError(
                f"({bin_file_path}) path contains non-ASCII characters (e.g. Chinese), "
                f"which may cause encoding issues. Please use an ASCII-only path."
            )
        raise RuntimeError(
            f"({bin_file_path}) file does not exist or permission denied!!!"
        )

    if not output_path:
        raise RuntimeError(
            f"({output_path}) directory does not exist or permission denied!!!"
        )
    if not all(ord(c) < 128 for c in output_path):
        raise RuntimeError(
            f"({output_path}) output path contains non-ASCII characters (e.g. Chinese), "
            f"which may cause encoding issues. Please use an ASCII-only path."
        )
    if os.path.exists(output_path):
        if not os.path.isdir(output_path):
            raise RuntimeError(f"({output_path}) is not a directory!!!")
    else:
        try:
            os.makedirs(output_path, exist_ok=True)
        except OSError as err:
            raise RuntimeError(
                f"({output_path}) failed to create directory: {err}"
            ) from err

    if not os.path.isfile(bin_file_path) and not os.path.isdir(bin_file_path):
        raise RuntimeError(f"({bin_file_path}) is neither a file nor a directory!!!")

    dump_bins = _collect_bin_files(bin_file_path)
    if not dump_bins:
        raise RuntimeError(f"({bin_file_path}) does not contain any .bin file!!!")

    parser_output_dir = None
    if os.path.isdir(bin_file_path):
        parser_output_dir = _make_parser_output_dir(output_path)
        DUMP_PARSER_LOG.set_log_file(os.path.join(parser_output_dir, "parser.log"))
        DUMP_PARSER_LOG.set_log_level(os.environ.get("ASCEND_GLOBAL_LOG_LEVEL", "3"))

    return dump_bins, output_path, parser_output_dir


def execute_parse():
    import sys

    param_len = len(sys.argv[1:])
    bin_file_path = ""
    output_path = os.getcwd()
    help_info = (
        "show_kernel_debug_data is a tool that parses dump binary data from AscendC debug API.\n"
        "It takes two inputs:\n   First mandatory param is binary file or directory\n"
        "   Second optional param is output path that stores result file,"
        " by default saving path is current directory.\n"
        "   ex: show_kernel_debug_data ./dump.bin ./output_dir"
    )
    if param_len == 2:
        bin_file_path, output_path = sys.argv[1:]
    elif param_len == 1 and sys.argv[1] in ["-h", "--help"]:
        print(help_info)
        return 0
    elif param_len == 1:
        bin_file_path = sys.argv[1]
    else:
        print(help_info)
        raise RuntimeError("parameters invalid, please check tool introduction.")

    dump_bins, output_path, parser_output_dir = _validate_and_prepare(
        bin_file_path, output_path
    )

    if parser_output_dir is not None:
        for dump_bin in dump_bins:
            ret = parse_dump_bin(
                dump_bin,
                output_path,
                parse_output_dir=parser_output_dir,
                init_logger=False,
            )
            if ret != 0:
                return ret
    else:
        ret = parse_dump_bin(dump_bins[0], output_path)
        if ret != 0:
            return ret
    return 0
