# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16.0)
project(npu_emulate)

set(npu_emulate_path ${CMAKE_CURRENT_SOURCE_DIR})

if(BUILD_OPEN_PROJECT)
    add_custom_target(npu_emulate-0.1.0-py3-none-any.whl ALL
        COMMAND ${CMAKE_COMMAND} -E chdir ${npu_emulate_path} ${HI_PYTHON} setup.py bdist_wheel
        COMMAND ${CMAKE_COMMAND} -E copy ${npu_emulate_path}/dist/npu_emulate*.whl ${CMAKE_INSTALL_PREFIX}/lib/npu_emulate-0.1.0-py3-none-any.whl
        COMMAND ${CMAKE_COMMAND} -E copy ${npu_emulate_path}/npu-emulate.sh ${CMAKE_INSTALL_PREFIX}/lib/npu-emulate
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${npu_emulate_path}/npu_emulate.egg-info ${npu_emulate_path}/build ${npu_emulate_path}/dist
    )
    install(PROGRAMS ${CMAKE_INSTALL_PREFIX}/lib/npu-emulate
        ${INSTALL_OPTIONAL} DESTINATION tools/npu_tools/bin
        PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
        COMPONENT asc-tools
    )
    install(PROGRAMS ${CMAKE_INSTALL_PREFIX}/lib/npu-emulate
        ${INSTALL_OPTIONAL} DESTINATION ${CMAKE_SYSTEM_PROCESSOR}-linux/bin
        PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        COMPONENT asc-tools
    )
    install(FILES ${CMAKE_INSTALL_PREFIX}/lib/npu_emulate-0.1.0-py3-none-any.whl
        ${INSTALL_OPTIONAL} DESTINATION tools/
        PERMISSIONS OWNER_READ GROUP_READ
        COMPONENT asc-tools
    )
else()
    add_custom_target(npu_emulate-0.1.0-py3-none-any.whl
        COMMAND ${CMAKE_COMMAND} -E chdir ${npu_emulate_path} ${HI_PYTHON} setup.py bdist_wheel
        COMMAND ${CMAKE_COMMAND} -E copy ${npu_emulate_path}/dist/npu_emulate*.whl ${CMAKE_INSTALL_PREFIX}/lib/npu_emulate-0.1.0-py3-none-any.whl
        COMMAND ${CMAKE_COMMAND} -E copy ${npu_emulate_path}/npu-emulate.sh ${CMAKE_INSTALL_PREFIX}/lib/npu-emulate
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${npu_emulate_path}/npu_emulate.egg-info ${npu_emulate_path}/build ${npu_emulate_path}/dist
        COMMAND echo "npu_emulate-0.1.0-py3-none-any.whl is ok!"
    )

    install(FILES ${CMAKE_INSTALL_PREFIX}/lib/npu-emulate
        ${INSTALL_OPTIONAL} DESTINATION lib
        PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
        COMPONENT asc-tools
    )
endif()
