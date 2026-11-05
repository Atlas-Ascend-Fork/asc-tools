// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "dbi/binary_instrumenter.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 7 && argc != 8) {
        std::cerr
            << "usage: real_tool_smoke <input> <output> <arch> <toolchain-root> <work-dir> <cache-dir> [ub-scalar]\n";
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary);
    const std::vector<uint8_t> image{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (image.empty()) {
        std::cerr << "input kernel is empty\n";
        return 1;
    }
    aclsan::BinaryInstrumentationConfig config{};
    config.arch = argv[3];
    config.probeGroups = {aclsan::ProbeGroup::Mte2};
    if (argc == 8) {
        if (std::string(argv[7]) != "ub-scalar") {
            std::cerr << "unknown collection mode: " << argv[7] << '\n';
            return 2;
        }
        config.probeGroups = {aclsan::ProbeGroup::Scalar};
    }
    config.toolchainRoot = argv[4];
    config.workDirectory = argv[5];
    config.cacheDirectory = argv[6];
    config.keepTemp = true;

    const auto result = aclsan::InstrumentBinary(config, image.data(), image.size());
    if (result.status != aclsan::BinaryInstrumentationStatus::Instrumented) {
        std::cerr << result.stage << ": " << result.diagnostic << '\n';
        return 1;
    }
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(result.binary.data()), static_cast<std::streamsize>(result.binary.size()));
    if (!output.good()) {
        std::cerr << "cannot write patched output: " << argv[2] << '\n';
        return 1;
    }
    std::cout << "trace_argument_offset=" << result.traceArgumentOffset << '\n';
    std::cout << argv[2] << '\n';
    return 0;
}
