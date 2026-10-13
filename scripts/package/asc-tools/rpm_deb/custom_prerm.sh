#!/bin/bash
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

sourcedir="${INSTALL_PATH}"
[ -z "${sourcedir}" ] && exit 0
pkg_arch_name=$(uname -m)
WHL_INSTALL_DIR_PATH="${sourcedir}/python/site-packages"
BIN_DIR_PATH="${sourcedir}/${pkg_arch_name}-linux/bin"
export PYTHONPATH="${WHL_INSTALL_DIR_PATH}"
export PIP_BREAK_SYSTEM_PACKAGES=1

remove_softlink() {
    local link_name="$1"

    [ -L "${BIN_DIR_PATH}/${link_name}" ] && rm -f "${BIN_DIR_PATH}/${link_name}"
    return 0
}

remove_msobjdump_softlink() {
    local link_path="${sourcedir}/tools/msobjdump/msobjdump"

    if [ -L "${link_path}" ] && [ "$(readlink "${link_path}")" = "npu-objdump" ]; then
        rm -f "${link_path}"
    fi
    return 0
}

remove_local_bin() {
    local bin_name="$1"

    [ -f "${WHL_INSTALL_DIR_PATH}/bin/${bin_name}" ] && rm -f "${WHL_INSTALL_DIR_PATH}/bin/${bin_name}"
    return 0
}

remove_package_leftovers() {
    local package_path="$1"

    [ -e "${package_path}" ] && rm -rf "${package_path}" 2>/dev/null
    return 0
}

remove_cpudebug_cmake_softlink() {
    local link_name="$1"
    local cmake_dir="${sourcedir}/tools/cpudebug/cmake"

    [ -L "${cmake_dir}/${link_name}" ] && rm -f "${cmake_dir}/${link_name}"
    return 0
}

remove_empty_delivery_dir() {
    local relative_dir="$1"

    rmdir "${sourcedir}/${relative_dir}" 2>/dev/null || true
    return 0
}

for pkg in mindstudio_opgen mindstudio_opst msobjdump show_kernel_debug_data optype_collector; do
    pip3 uninstall -y "${pkg}" >/dev/null 2>&1 || true
done

remove_softlink "msopgen"
remove_softlink "msopst"
remove_softlink "optype_collector"
remove_msobjdump_softlink

remove_cpudebug_cmake_softlink "tikicpulib-config.cmake"
remove_cpudebug_cmake_softlink "targets-tikicpulib.cmake"
remove_cpudebug_cmake_softlink "targets-tikicpulib-release.cmake"

remove_empty_delivery_dir "${pkg_arch_name}-linux/include/version"
remove_empty_delivery_dir "tools/cpudebug/lib64/Kirin9030"
remove_empty_delivery_dir "tools/cpudebug/lib64/KirinX90"
remove_empty_delivery_dir "tools/msopgen"

for bin_name in msopgen msopst optype_collector op_ut_run op_ut_helper msopst.ini mindstudio_opgen; do
    remove_local_bin "${bin_name}"
done

remove_package_leftovers "${WHL_INSTALL_DIR_PATH}/msobjdump"
remove_package_leftovers "${WHL_INSTALL_DIR_PATH}/msopgen"
remove_package_leftovers "${WHL_INSTALL_DIR_PATH}/msopst"
remove_package_leftovers "${WHL_INSTALL_DIR_PATH}/show_kernel_debug_data"
remove_package_leftovers "${WHL_INSTALL_DIR_PATH}/optype_collector"

rm -fr "${WHL_INSTALL_DIR_PATH}"/mindstudio_opgen-*.dist-info 2>/dev/null
rm -fr "${WHL_INSTALL_DIR_PATH}"/mindstudio_opst-*.dist-info 2>/dev/null
rm -fr "${WHL_INSTALL_DIR_PATH}"/msobjdump-*.dist-info 2>/dev/null
rm -fr "${WHL_INSTALL_DIR_PATH}"/show_kernel_debug_data-*.dist-info 2>/dev/null
rm -fr "${WHL_INSTALL_DIR_PATH}"/optype_collector-*.dist-info 2>/dev/null

rmdir "${WHL_INSTALL_DIR_PATH}/bin" 2>/dev/null || true
rmdir "${WHL_INSTALL_DIR_PATH}" 2>/dev/null || true
rmdir "${sourcedir}/python" 2>/dev/null || true
