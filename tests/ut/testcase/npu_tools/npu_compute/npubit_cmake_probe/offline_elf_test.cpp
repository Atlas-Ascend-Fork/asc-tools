/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <npubit.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdint>
#include <limits>
#include <string>
int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<char> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (image.empty()) {
        std::fprintf(stderr, "empty ELF\n");
        return 2;
    }
    NpubitHandle handle = nullptr;
    auto status = NpubitCreateHandle(image.data(), image.size(), &handle, V100, true);
    std::printf("create status=%d handle=%s\n", int(status), handle ? "valid" : "null");
    if (status != NPUBIT_SUCCESS || !handle)
        return 1;
    NpubitHwContext hw{1, 32};
    status = NpubitSetHardwareContext(handle, &hw);
    std::printf("hardware status=%d\n", int(status));
    if (status != NPUBIT_SUCCESS) {
        NpubitReleaseHandle(handle);
        return 1;
    }
    void* bb = nullptr;
    std::size_t bbsize = 0;
    status = NpubitAllocateBasicBlockBuffer(handle, &bb, &bbsize);
    std::printf("bb map status=%d bytes=%zu\n", int(status), bbsize);
    if (status != NPUBIT_SUCCESS) {
        NpubitReleaseHandle(handle);
        return 1;
    }
    std::size_t size = 0;
    status = NpubitGetBufferSize(handle, NPUBIT_SIMT_WARP_OCCUPANCY, &size);
    std::printf("buffer status=%d bytes=%zu\n", int(status), size);
    if (status != NPUBIT_SUCCESS) {
        if (bb)
            NpubitFree(&bb);
        NpubitReleaseHandle(handle);
        return 1;
    }
    if (size == std::numeric_limits<std::size_t>::max())
        size = 4 * 1024 * 1024;
    std::string args;
    if (bbsize)
        args = "bb-map-address:8589934592,bb-map-section-size:" + std::to_string(bbsize);
    // Offline SDK transformation only. Fake addresses are never executed on a device.
    void* fakeDeviceAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x100000000ULL));
    status = NpubitInstrument(
        handle, NPUBIT_SIMT_WARP_OCCUPANCY, fakeDeviceAddress, size, args.empty() ? nullptr : args.c_str());
    std::printf("instrument status=%d\n", int(status));
    if (status == NPUBIT_SUCCESS && bbsize) {
        status = NpubitInitializeBasicBlockBufferMap(handle, bb, bbsize);
        std::printf("bb initialize status=%d\n", int(status));
    }
    std::size_t outsize = 0;
    if (status == NPUBIT_SUCCESS) {
        status = NpubitGetObjectSize(handle, NPUBIT_OBJECT_INSTRUMENTED, &outsize);
        std::printf("object size status=%d bytes=%zu\n", int(status), outsize);
    }
    if (status == NPUBIT_SUCCESS && outsize > 0 && outsize < 64 * 1024 * 1024) {
        std::vector<char> out(outsize);
        status = NpubitGetObject(handle, NPUBIT_OBJECT_INSTRUMENTED, out.data(), out.size());
        std::printf(
            "object get status=%d elf=%d\n", int(status),
            out.size() > 4 && out[0] == 0x7f && out[1] == 'E' && out[2] == 'L' && out[3] == 'F');
        if (status == NPUBIT_SUCCESS &&
            !(out.size() > 4 && out[0] == 0x7f && out[1] == 'E' && out[2] == 'L' && out[3] == 'F'))
            status = NPUBIT_PASS_EXECUTION_FAILURE;
    } else if (status == NPUBIT_SUCCESS)
        status = NPUBIT_PASS_EXECUTION_FAILURE;
    if (bb)
        NpubitFree(&bb);
    NpubitReleaseHandle(handle);
    return status == NPUBIT_SUCCESS ? 0 : 1;
}
