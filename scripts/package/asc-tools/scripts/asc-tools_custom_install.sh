#!/bin/bash
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
SHELL_DIR=$(cd "$(dirname "$0")" || exit;pwd)
COMMON_SHELL_PATH="$SHELL_DIR/common.sh"
LOG_PATH="/var/log/ascend_seclog/ascend_install.log"
PACKAGE=asc-tools
LEVEL_INFO="INFO"
LEVEL_WARN="WARNING"
LEVEL_ERROR="ERROR"
MIN_PIP_VERSION=18
PLT_ARCH=$(uname -m)

source "${COMMON_SHELL_PATH}"

username=$(id -un)
usergroup=$(groups | cut -d" " -f1)

# 查找 whl 文件（支持通配符）
find_whl_file() {
    local _dir="$1"
    local _pattern="$2"
    local _found=$(ls ${_dir}/${_pattern} 2>/dev/null | head -n 1)
    if [ -n "$_found" ]; then
        echo "$_found"
        return 0
    fi
    return 1
}

log() {
    local content=`echo "$@" | cut -d" " -f2-`
    cur_date=`date +"%Y-%m-%d %H:%M:%S"`
    echo "[AscTools] [${cur_date}] [$1]: $content" >> "${log_file}"
}

log_and_print() {
    local content=`echo "$@" | cut -d" " -f2-`
    cur_date=`date +"%Y-%m-%d %H:%M:%S"`
    echo "[AscTools] [${cur_date}] [$1]: $content"
    echo "[AscTools] [${cur_date}] [$1]: $content" >> "${log_file}"
}

checkPip() {
    pip_info=$(pip3 --version 2>/dev/null)
    if [ $? -ne 0 ] || [ -z "${pip_info}" ]; then
        log_and_print $LEVEL_ERROR "execute pip3 --version failed, please check."
        return 1
    fi
    version=$(echo "${pip_info}" | cut -d" " -f2 | cut -d"." -f1)
    if [ ${version} -lt ${MIN_PIP_VERSION} ]; then
        log_and_print $LEVEL_WARN "pip3 version low than ${MIN_PIP_VERSION}.0"
        log_and_print $LEVEL_WARN "Current pip3 version may cause the installation problems."
    fi
    curpath="$(dirname ${BASH_SOURCE:-$0})"
    install_dir="$(realpath $curpath/..)"
    common_interface=$(realpath $install_dir/script*/common_interface.sh)
    if [ -f "$common_interface" ]; then
        . "$common_interface"
        py_version_check
    else
        log_and_print $LEVEL_ERROR "Failed to find common_interface.sh file to check python version."
        return 1
    fi
    return 0
}

installWhlPackage() {
    local _package=$1
    local _pythonlocalpath=$2

    changeFileMode 750 ${_pythonlocalpath}
    if [ ! -f "${_package}" ]; then
        log_and_print ${LEVEL_ERROR} "ERR_NO:0x0080;ERR_DES: The ${_package} does not exist."
        return 1
    fi
    if [ "-${pylocal}" = "-y" ]; then
        pip3 install --upgrade --no-deps --force-reinstall --disable-pip-version-check "${_package}" -t "${_pythonlocalpath}" >> "${log_file}" 2>&1
    else
        if [ "$(id -u)" -ne 0 ]; then
            pip3 install --upgrade --no-deps --force-reinstall --disable-pip-version-check "${_package}" --user >> "${log_file}" 2>&1
        else
            pip3 install --upgrade --no-deps --force-reinstall --disable-pip-version-check "${_package}" >> "${log_file}" 2>&1
        fi
    fi
    if [ $? -ne 0 ]; then
        log_and_print ${LEVEL_ERROR} "Install ${_package} failed. For more details, please read ${log_file} file."
        return 1
    fi
    log ${LEVEL_INFO} "install ${_package} succeed."
    return 0
}

installMsprofWhlPackage() {
    local _package=$1
    local _python_local_path=$2

    log ${LEVEL_INFO} "start to begin install ${_package}."
    log ${LEVEL_INFO} "The installation path ${_python_local_path} of whl package"
    if [ ! -f "${_package}" ]; then
        log_and_print ${LEVEL_ERROR} "ERR_NO:0x0080;ERR_DES: The ${_package} does not exist."
        return 1
    fi

    pip3 install --upgrade --no-deps --force-reinstall --disable-pip-version-check "${_package}" -t "${_python_local_path}" > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_and_print ${LEVEL_ERROR} "Install ${_package} failed."
        return 1
    fi
    log ${LEVEL_INFO} "install ${_package} succeed."
    return 0
}

installOpPython() {
    checkAscendcFeature ${feature_type}
    [ $? -ne 0 ] && return 0

    log_and_print $LEVEL_INFO "begin to install ascendc python whl."
    if [ "-${pylocal}" = "-y" ]; then
        createPythonLocalDir "$install_path"
        [ $? -ne 0 ] && return 1
        installOpPythonLocal
        [ $? -ne 0 ] && return 1
        return 0
    fi

    installWhlPackage "${install_path}/tools/msobjdump-0.1.0-py3-none-any.whl"
    if [ $? -ne 0 ]; then
        return 1
    fi

    installWhlPackage "${install_path}/tools/show_kernel_debug_data-0.1.0-py3-none-any.whl"
    if [ $? -ne 0 ]; then
        return 1
    fi

    installWhlPackage "${install_path}/tools/optype_collector-0.1.0-py3-none-any.whl"
    if [ $? -ne 0 ]; then
        return 1
    fi

    local _opgen_whl=$(find_whl_file "${install_path}/tools" "mindstudio_opgen-*-py3-none-any.whl")
    if [ -n "$_opgen_whl" ]; then
        installWhlPackage "$_opgen_whl"
        if [ $? -ne 0 ]; then
            return 1
        fi
    else
        log_and_print $LEVEL_ERROR "mindstudio_opgen whl file not found"
        return 1
    fi

    local _opst_whl=$(find_whl_file "${install_path}/tools" "mindstudio_opst-*-py3-none-any.whl")
    if [ -n "$_opst_whl" ]; then
        installWhlPackage "$_opst_whl"
        if [ $? -ne 0 ]; then
            return 1
        fi
    else
        log_and_print $LEVEL_ERROR "mindstudio_opst whl file not found"
        return 1
    fi

    return 0
}

installAllPython() {
    checkAllFeature ${feature_type}
    [ $? -ne 0 ] && return 0

    if [ "-${pylocal}" = "-y" ]; then
        createPythonLocalDir "$install_path"
        [ $? -ne 0 ] && return 1
        installAllPythonLocal
        [ $? -ne 0 ] && return 1
        return 0
    fi

    installWhlPackage "${install_path}/tools/msobjdump-0.1.0-py3-none-any.whl"
    if [ $? -ne 0 ]; then
        return 1
    fi

    installWhlPackage "${install_path}/tools/show_kernel_debug_data-0.1.0-py3-none-any.whl"
    if [ $? -ne 0 ]; then
        return 1
    fi

    installWhlPackage "${install_path}/tools/optype_collector-0.1.0-py3-none-any.whl"
    if [ $? -ne 0 ]; then
        return 1
    fi

    local _opgen_whl=$(find_whl_file "${install_path}/tools" "mindstudio_opgen-*-py3-none-any.whl")
    if [ -n "$_opgen_whl" ]; then
        installWhlPackage "$_opgen_whl"
        if [ $? -ne 0 ]; then
            return 1
        fi
    else
        log_and_print $LEVEL_ERROR "mindstudio_opgen whl file not found"
        return 1
    fi

    local _opst_whl=$(find_whl_file "${install_path}/tools" "mindstudio_opst-*-py3-none-any.whl")
    if [ -n "$_opst_whl" ]; then
        installWhlPackage "$_opst_whl"
        if [ $? -ne 0 ]; then
            return 1
        fi
    else
        log_and_print $LEVEL_ERROR "mindstudio_opst whl file not found"
        return 1
    fi

    return 0
}

installPython() {
    checkPip
    [ $? -ne 0 ] && return 1

    installOpPython
    [ $? -ne 0 ] && return 1

    installAllPython
    [ $? -ne 0 ] && return 1

    return 0
}

createPythonLocalDir() {
    local _dst_path="$1"
    local _py_path="$_dst_path/python"
    local _py_pkg_path="$_dst_path/python/site-packages"

    if [ ! -d "$_py_path" ]; then
        createFolder "$_py_path" "$username:$usergroup" 750
        if [ $? -ne 0 ]; then
            log_and_print $LEVEL_ERROR "Create $_py_path folder failed."
            return 1
        fi
    fi
    if [ ! -d "$_py_pkg_path" ]; then
        createFolder "$_py_pkg_path" "$username:$usergroup" 750
        if [ $? -ne 0 ]; then
            log_and_print $LEVEL_ERROR "Create $_py_pkg_path folder failed."
            return 1
        fi
    fi
    return 0
}

installPyWhlLocal() {
    local _whl_pattern="$1"
    local _py_whl_path
    # 支持通配符模式查找 whl 文件
    if [[ "$_whl_pattern" == *"*"* ]]; then
        _py_whl_path=$(find_whl_file "${install_path}/tools" "$_whl_pattern")
        if [ -z "$_py_whl_path" ]; then
            log_and_print ${LEVEL_ERROR} "No whl file found matching pattern: $_whl_pattern"
            return 1
        fi
    else
        _py_whl_path="$install_path/tools/$_whl_pattern"
    fi
    local _py_install_path="$install_path/python/site-packages"

    local _temp_install_path="$_py_install_path/temp"
    # install python package to temp
    installWhlPackage "$_py_whl_path" "$_temp_install_path"
    [ $? -ne 0 -o ! -d "$_temp_install_path" ] && return 1

    changeDirMode 750 "$_temp_install_path"
    changeFileMode 750 "$_temp_install_path"
    chown -RP "$username:$usergroup" "$_temp_install_path">/dev/null 2>&1

    # copy temp to dest path
    local _py_dirs=$(ls "$_temp_install_path")
    for dir in ${_py_dirs[@]}; do
        if [ "-$dir" = "-bin" ]; then
            if [ -d "$_py_install_path/bin" ]; then
                cp -Rfp "$_temp_install_path/bin"/* "$_py_install_path/bin"
                continue
            fi
        fi
        cp -Rfp "$_temp_install_path/$dir" "$_py_install_path"
    done

    # remove temp
    rm -rf "$_temp_install_path"
    return 0
}

installOpPythonLocal() {
    installPyWhlLocal "msobjdump-0.1.0-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    installPyWhlLocal "show_kernel_debug_data-0.1.0-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    installPyWhlLocal "optype_collector-0.1.0-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    installPyWhlLocal "mindstudio_opgen-*-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    installPyWhlLocal "mindstudio_opst-*-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    log $LEVEL_INFO "Install operator python package succeed."
    return 0
}

installAllPythonLocal() {
    installPyWhlLocal "msobjdump-0.1.0-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    installPyWhlLocal "show_kernel_debug_data-0.1.0-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    installPyWhlLocal "optype_collector-0.1.0-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    installPyWhlLocal "mindstudio_opgen-*-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    installPyWhlLocal "mindstudio_opst-*-py3-none-any.whl"
    [ $? -ne 0 ] && return 1

    log $LEVEL_INFO "Install all module python package succeed."
    return 0
}

init() {
    [ ! -d "${install_path}" ] && exit 1

    if [ ! -z "${version_dir}" ]; then
        install_path="${install_path}/${version_dir}"
        [ ! -d "${install_path}" ] && exit 1
    fi

    if [ $(id -u) -eq 0 ]; then
        log_file=${LOG_PATH}
    else
        local _home_path=$(eval echo "~")
        log_file="${_home_path}/${LOG_PATH}"
    fi
}

createSoftLink()
{
    local _src_dir="$1"
    local _dst_dir="$2"
    local _name="$3"

    [ ! -d "$_src_dir" -o ! -d "$_dst_dir" ] && return
    [ ! -f "$_src_dir/$_name" -a ! -d "$_src_dir/$_name" ] && return

    if [ -L "$_dst_dir/$_name" ]; then
        rm -rf "$_dst_dir/$_name"
    fi

    ln -s "$_src_dir/$_name" "$_dst_dir/$_name"
}

log_file=""
is_quiet=n
pylocal=n
install_path=""
version_dir=""
feature_type=""

while true; do
    case "$1" in
    --install-path=*)
        install_path=$(echo "$1" | cut -d"=" -f2-)
        [ -z "${install_path}" ] && exit 1
        shift
        ;;
    --version-dir=*)
        version_dir=$(echo "$1" | cut -d"=" -f2-)
        shift
        ;;
    --quiet=*)
        is_quiet=$(echo "$1" | cut -d"=" -f2)
        shift
        ;;
    --pylocal=*)
        pylocal=$(echo "$1" | cut -d"=" -f2)
        shift
        ;;
    --feature=*)
        feature_type=$(echo "$1" | cut -d"=" -f2)
        shift
        ;;
    -*)
        shift
        ;;
    *)
        break
        ;;
    esac
done

init

installPython
[ $? -ne 0 ] && exit 1

createSoftLink "${install_path}/python/site-packages/bin" "${install_path}/${PLT_ARCH}-linux/bin/" "msopgen"
createSoftLink "${install_path}/python/site-packages/bin" "${install_path}/${PLT_ARCH}-linux/bin/" "msopst"
createSoftLink "${install_path}/python/site-packages/bin" "${install_path}/${PLT_ARCH}-linux/bin/" "optype_collector"

exit 0
