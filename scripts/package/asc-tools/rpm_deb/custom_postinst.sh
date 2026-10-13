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
WHL_DIR_PATH="${sourcedir}/tools"
WHL_INSTALL_DIR_PATH="${sourcedir}/python/site-packages"
BIN_DIR_PATH="${sourcedir}/${pkg_arch_name}-linux/bin"
unset PYTHONPATH
export PIP_BREAK_SYSTEM_PACKAGES=1

is_python_usable() {
    "$1" -m pip --version >/dev/null 2>&1
}

find_python() {
    if [ -n "${PYTHON_BIN:-}" ] && is_python_usable "${PYTHON_BIN}"; then
        return 0
    fi

    for candidate in ${PYTHON:-} ${HI_PYTHON:-} python3 /opt/conda/bin/python3 /usr/local/bin/python3 /usr/bin/python3 /opt/conda/envs/*/bin/python3; do
        [ -z "${candidate}" ] && continue
        if command -v "${candidate}" >/dev/null 2>&1; then
            resolved_python=$(command -v "${candidate}")
        elif [ -x "${candidate}" ]; then
            resolved_python="${candidate}"
        else
            continue
        fi

        if is_python_usable "${resolved_python}"; then
            PYTHON_BIN="${resolved_python}"
            return 0
        fi
    done

    return 1
}

run_pip() {
    if find_python; then
        "${PYTHON_BIN}" -m pip "$@"
        return $?
    fi

    pip3 "$@"
}

find_whl_file() {
    local whl_pattern="$1"
    ls "${WHL_DIR_PATH}"/${whl_pattern} 2>/dev/null | head -1
}

merge_whl_install() {
    local temp_install_path="$1"
    local item
    local item_name

    for item in "${temp_install_path}"/*; do
        [ -e "${item}" ] || continue
        item_name=$(basename "${item}")

        if [ "${item_name}" = "bin" ] && [ -d "${WHL_INSTALL_DIR_PATH}/bin" ]; then
            find "${item}" -mindepth 1 -maxdepth 1 -exec cp -Rfp {} "${WHL_INSTALL_DIR_PATH}/bin" \;
            continue
        fi

        cp -Rfp "${item}" "${WHL_INSTALL_DIR_PATH}"
    done
}

set_tree_mode() {
    local mode="$1"
    local path="$2"

    [ -e "${path}" ] || return 0
    find "${path}" ! -type l -exec chmod "${mode}" {} + 2>/dev/null || true
}

set_dir_mode() {
    local mode="$1"
    local path="$2"

    [ -d "${path}" ] || return 0
    chmod "${mode}" "${path}" 2>/dev/null || true
}

set_run_compatible_permissions() {
    set_tree_mode 755 "${WHL_INSTALL_DIR_PATH}"
    set_tree_mode 500 "${sourcedir}/share/info/asc-tools/script"

    set_dir_mode 555 "${sourcedir}/${pkg_arch_name}-linux/include/version"
    set_dir_mode 555 "${sourcedir}/tools/cpudebug/cmake"
    set_dir_mode 555 "${sourcedir}/tools/cpudebug/lib64/Kirin9030"
    set_dir_mode 555 "${sourcedir}/tools/cpudebug/lib64/KirinX90"
    set_dir_mode 555 "${sourcedir}/tools/msopgen"
}

install_whl() {
    local whl_pattern="$1"
    local whl_path
    local temp_install_path

    whl_path=$(find_whl_file "${whl_pattern}")
    if [ -z "${whl_path}" ]; then
        echo "[asc-tools] ${whl_pattern} not found, skip install"
        return 0
    fi

    echo "[asc-tools] installing ${whl_path}"
    temp_install_path=$(mktemp -d "${WHL_INSTALL_DIR_PATH}/.whl_install.XXXXXX")
    if ! run_pip install --disable-pip-version-check --upgrade --no-deps --force-reinstall -t "${temp_install_path}" "${whl_path}"; then
        rm -rf "${temp_install_path}"
        return 1
    fi

    set_tree_mode 755 "${temp_install_path}"
    merge_whl_install "${temp_install_path}"
    local ret=$?
    rm -rf "${temp_install_path}"
    return ${ret}
}

create_softlink() {
    local link_name="$1"

    [ ! -d "${WHL_INSTALL_DIR_PATH}/bin" ] && return 0
    [ ! -d "${BIN_DIR_PATH}" ] && return 0
    [ ! -f "${WHL_INSTALL_DIR_PATH}/bin/${link_name}" ] && return 0

    [ -L "${BIN_DIR_PATH}/${link_name}" ] && rm -f "${BIN_DIR_PATH}/${link_name}"
    ln -s "${WHL_INSTALL_DIR_PATH}/bin/${link_name}" "${BIN_DIR_PATH}/${link_name}" || true
}

create_msobjdump_softlink() {
    local tool_dir="${sourcedir}/tools/msobjdump"
    local link_path="${tool_dir}/msobjdump"

    [ ! -f "${tool_dir}/npu-objdump" ] && return 0
    if [ -e "${link_path}" ] && [ ! -L "${link_path}" ]; then
        return 0
    fi
    if [ -L "${link_path}" ] && [ "$(readlink "${link_path}")" != "npu-objdump" ]; then
        return 0
    fi
    [ -L "${link_path}" ] || ln -s "npu-objdump" "${link_path}"
}

create_cpudebug_cmake_softlink() {
    local target_name="$1"
    local link_name="$2"
    local cmake_dir="${sourcedir}/tools/cpudebug/cmake"

    [ ! -d "${cmake_dir}" ] && return 0
    [ ! -f "${cmake_dir}/${target_name}" ] && return 0

    ln -sfn "${target_name}" "${cmake_dir}/${link_name}"
}

create_empty_delivery_dir() {
    local relative_dir="$1"

    mkdir -p "${sourcedir}/${relative_dir}"
}

create_empty_delivery_dirs() {
    create_empty_delivery_dir "${pkg_arch_name}-linux/include/version"
    create_empty_delivery_dir "tools/cpudebug/lib64/Kirin9030"
    create_empty_delivery_dir "tools/cpudebug/lib64/KirinX90"
    create_empty_delivery_dir "tools/msopgen"
}

remove_run_install_entrypoints() {
    local script_dir

    for script_dir in "${sourcedir}/share/info/asc-tools/script"; do
        [ -d "${script_dir}" ] || continue
        rm -f "${script_dir}/install.sh" "${script_dir}/run_asc-tools_install.sh"
        rmdir "${script_dir}" 2>/dev/null || true
    done
}

create_empty_delivery_dirs
remove_run_install_entrypoints
mkdir -p "${WHL_INSTALL_DIR_PATH}"

install_whl "msobjdump-0.1.0-py3-none-any.whl"
install_whl "show_kernel_debug_data-0.1.0-py3-none-any.whl"
install_whl "optype_collector-0.1.0-py3-none-any.whl"
install_whl "mindstudio_opgen-*-py3-none-any.whl"
install_whl "mindstudio_opst-*-py3-none-any.whl"

create_softlink "msopgen"
create_softlink "msopst"
create_softlink "optype_collector"
create_msobjdump_softlink

create_cpudebug_cmake_softlink "cpudebug-config.cmake" "tikicpulib-config.cmake"
create_cpudebug_cmake_softlink "targets-cpudebug.cmake" "targets-tikicpulib.cmake"
create_cpudebug_cmake_softlink "targets-cpudebug-release.cmake" "targets-tikicpulib-release.cmake"

set_run_compatible_permissions
