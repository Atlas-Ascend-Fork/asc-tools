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
COMMON_INC_PATH="$SHELL_DIR/common_func.inc"
FILELIST_PATH="$SHELL_DIR/filelist.csv"
VERSION_INFO="$SHELL_DIR/../version.info"
PACKAGE=asc-tools
USERNAME=$(id -un)
USERGROUP=$(groups | cut -d" " -f1)

source "${COMMON_SHELL_PATH}"
source "${COMMON_INC_PATH}"

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

createPythonSoftLink()
{
    local _install_path=$1
    local _version_dir=$2
    local _latest_dir=$3
    local _src_dir="$_install_path/$_version_dir/python/site-packages"
    local _dst_dir="$_install_path/$_latest_dir/python/site-packages"

    [ -z "$_version_dir" ] && return
    [ ! -d "$_src_dir" ] && return

    if [ ! -d "$_install_path/$_latest_dir/python" ]; then
        createFolder "$_install_path/$_latest_dir/python" $USERNAME:$USERGROUP 750
        [ $? -ne 0 ] && return
    fi
    if [ ! -d "$_install_path/$_latest_dir/python/site-packages/" ]; then
        createFolder "$_install_path/$_latest_dir/python/site-packages/" $USERNAME:$USERGROUP 750
        [ $? -ne 0 ] && return
    fi

    createSoftLink "$_src_dir" "$_dst_dir" "msobjdump"
    createSoftLink "$_src_dir" "$_dst_dir" "msobjdump-0.1.0.dist-info"
    createSoftLink "$_src_dir" "$_dst_dir" "show_kernel_debug_data"
    createSoftLink "$_src_dir" "$_dst_dir" "show_kernel_debug_data-0.1.0.dist-info"
    createSoftLink "$_src_dir" "$_dst_dir" "optype_collector"
    createSoftLink "$_src_dir" "$_dst_dir" "optype_collector-0.1.0.dist-info"
}

get_arch_name() {
    local pkg_dir="$1"
    local scene_file="$pkg_dir/scene.info"
    grep '^arch=' $scene_file | cut -d"=" -f2
}

do_create_stub_softlink() {
    local install_path=$1
    local version_dir=$2

    local arch_name="$(get_arch_name $install_path/$version_dir/share/info/asc-tools)"
    local arch_linux_path="$install_path/$version_dir/$arch_name-linux"
    if [ ! -e "$arch_linux_path" ] || [ -L "$arch_linux_path" ]; then
        return
    fi

    local pwdbak="$(pwd)"
    cd $install_path/$version_dir/tools && ln -sf "../$arch_name-linux/simulator" "simulator"
    cd $pwdbak
}

createToolSoftLink()
{
    local _install_path=$1
    local _version_dir=$2
    local _latest_dir=$3
    local _src_dir="$_install_path/$_version_dir/tools"
    local _dst_dir="$_install_path/$_latest_dir/tools"

    [ -z "$_version_dir" ] && return
    [ ! -d "$_src_dir" ] && return

    [ ! -f "$FILELIST_PATH" ] && return
    local _tool_files=$(cat "$FILELIST_PATH" | cut -d',' -f4 | grep "^tools/[^/]\+$" | cut -d"/" -f2 | sort | uniq)
    for tool_file in ${_tool_files[@]}; do
        [ -L "$_src_dir/$tool_file" ] && continue
        createSoftLink "$_src_dir" "$_dst_dir" "$tool_file"
    done
    if [ -d "$_src_dir/msobjdump" ] && [ -d "$_dst_dir/msobjdump" ]; then
        createSoftLink "$_src_dir/msobjdump" "$_dst_dir/msobjdump" "npu-objdump"
        createSoftLink "$_src_dir/msobjdump" "$_dst_dir/msobjdump" "msobjdump"
    fi
    if [ -d "$install_path/$version_dir/tools" ]; then
        if [ ! -d "$install_path/$latest_dir/tools" ]; then
            mkdir -p "$install_path/$latest_dir/tools"
        fi
        if [ ! -d "$install_path/$latest_dir/tools/ascendc_tools" ]; then
            mkdir -p "$install_path/$latest_dir/tools/ascendc_tools"
        fi
        if [ ! -L "$install_path/$latest_dir/tools/ascendc_tools/ascendc_npuchk_report.py" ]; then
            if [ ! -e "$install_path/$latest_dir/tools/ascendc_tools/ascendc_npuchk_report.py" ]; then
                ln -sr "$install_path/$version_dir/tools/ascendc_tools/ascendc_npuchk_report.py" "$install_path/$latest_dir/tools/ascendc_tools/ascendc_npuchk_report.py"
            fi
        fi
        if [ ! -d "$install_path/$latest_dir/tools/msobjdump" ]; then
            ln -sr "$install_path/$version_dir/tools/msobjdump" "$install_path/$latest_dir/tools/msobjdump"
        fi
        if [ ! -d "$install_path/$latest_dir/tools/optype_collector" ]; then
            ln -sr "$install_path/$version_dir/tools/optype_collector" "$install_path/$latest_dir/tools/optype_collector"
        fi
        if [ ! -d "$install_path/$latest_dir/tools/show_kernel_debug_data" ]; then
            ln -sr "$install_path/$version_dir/tools/show_kernel_debug_data" "$install_path/$latest_dir/tools/show_kernel_debug_data"
        fi
        if [ ! -d "$install_path/$latest_dir/tools/cpudebug" ]; then
            ln -sr "$install_path/$version_dir/tools/cpudebug" "$install_path/$latest_dir/tools/cpudebug"
        fi
        if [ ! -d "$install_path/$latest_dir/tools/tikicpulib" ]; then
            ln -sr "$install_path/$version_dir/tools/tikicpulib" "$install_path/$latest_dir/tools/tikicpulib"
        fi

        do_create_stub_softlink "$install_path" "$version_dir"

        if [ ! -d "$install_path/$latest_dir/tools/simulator" ]; then
            ln -sr "$install_path/$version_dir/tools/simulator" "$install_path/$latest_dir/tools/simulator"
        fi
    fi
}

createCanndevSoft() {
    local _install_path=$1
    local _version_dir=$2
    local _latest_dir=$3
    local _src_dir="$_install_path/$_version_dir/python/site-packages/bin"
    local _dst_dir="$_install_path/$_latest_dir/bin"

    [ -z "$_version_dir" ] && return
    [ ! -d "$_src_dir" ] && return
    [ ! -d "$_dst_dir" ] && mkdir -p "$_dst_dir"

    createSoftLink "$_src_dir" "$_dst_dir" "msopgen"
    createSoftLink "$_src_dir" "$_dst_dir" "msopst"
    createSoftLink "$_src_dir" "$_dst_dir" "optype_collector"
}

install_path=""
version_dir=""
latest_dir=""

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
    --latest-dir=*)
        latest_dir=$(echo "$1" | cut -d"=" -f2-)
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

is_multi_version_pkg "is_multi_version" "${VERSION_INFO}"
[ ! "$is_multi_version" = "true" ] && exit 0

createPythonSoftLink "$install_path" "$version_dir" "$latest_dir"
createToolSoftLink "$install_path" "$version_dir" "$latest_dir"
createCanndevSoft "$install_path" "$version_dir" "$latest_dir"

exit 0
