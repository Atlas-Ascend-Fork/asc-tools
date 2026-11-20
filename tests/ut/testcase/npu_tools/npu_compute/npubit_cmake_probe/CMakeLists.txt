# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
add_executable(npubit_version_probe version_probe.cpp)
target_link_libraries(npubit_version_probe PRIVATE npu_bit)
add_test(NAME npu_compute.npubit_version COMMAND npubit_version_probe)

add_executable(npubit_environment_test environment_test.cpp)
target_include_directories(npubit_environment_test PRIVATE "${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling")
target_link_libraries(npubit_environment_test PRIVATE npu_bit)
target_compile_options(npubit_environment_test PRIVATE -UNDEBUG)
add_test(NAME npu_compute.npubit_environment COMMAND npubit_environment_test)

# Instrumentation on the host only. These device addresses are never used for a launch.
add_executable(npubit_offline_elf_test offline_elf_test.cpp)
target_link_libraries(npubit_offline_elf_test PRIVATE npu_bit)
if(NPUBIT_TEST_ELF)
  add_test(NAME npu_compute.npubit_offline_elf COMMAND npubit_offline_elf_test "${NPUBIT_TEST_ELF}")
endif()
