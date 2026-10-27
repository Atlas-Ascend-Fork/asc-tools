#!/bin/bash
# -----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# -----------------------------------------------------------------------------------------------------------
set -x
cd ${WORKSPACE}
echo $(grep -E "^VERSION_ID=" /etc/os-release | cut -d'"' -f2)
if sudo update-alternatives --set gcc /usr/bin/gcc-16 2>/dev/null; then
    echo "Switched to gcc-16"
elif sudo update-alternatives --set gcc /usr/bin/gcc-15 2>/dev/null; then
    echo "Switched to gcc-15"
elif sudo update-alternatives --set gcc /usr/bin/gcc-14 2>/dev/null; then
    echo "gcc-16/15 not available, fell back to gcc-14"
fi
if gcc --version | head -n1 | grep -q "15\."; then
    rm -rf /home/jenkins/opensource/lib_cache
    if [ -d /home/jenkins/opensource/gcc15 ]; then
        rm -rf /home/jenkins/opensource/gcc15/lib_cache/abseil-cpp
        rm -rf /home/jenkins/opensource/gcc15/lib_cache/device/abseil-cpp
        ln -s /home/jenkins/opensource/gcc15/lib_cache/ /home/jenkins/opensource/lib_cache
    elif [ -d /home/jenkins/opensource/gcc15x86 ]; then
        rm -rf /home/jenkins/opensource/gcc15x86/lib_cache/abseil-cpp
        rm -rf /home/jenkins/opensource/gcc15x86/lib_cache/device/abseil-cpp
        ln -s /home/jenkins/opensource/gcc15x86/lib_cache/ /home/jenkins/opensource/lib_cache
    fi
elif gcc --version | head -n1 | grep -q "14\."; then
    gcc --version
else
    gcc --version
    rm -rf /home/jenkins/opensource/lib_cache
    ln -s /home/jenkins/opensource/ubuntu20/lib_cache /home/jenkins/opensource/lib_cache
fi
source /home/jenkins/Ascend/cann/bin/setenv.bash
set +e
if [ "$TARGET_BRANCH" = "master" ];then
    case "${ut_type}" in
        UT_Test_Python)
            bash build.sh --python_utest --cov --cann_3rd_lib_path="/home/jenkins/opensource"
            ret=$?
            coverage_save="true"
            ;;
        UT_Test)
            bash build.sh --cpp_utest --cov --cann_3rd_lib_path="/home/jenkins/opensource"
            ret=$?
            ;;
    esac
else
    case "${ut_type}" in
        UT_Test_Python)
            echo "Skip UT test execution for UT_Test_Python on non-master branch"
            exit 0
            ;;
        UT_Test)
            bash build.sh -t --cann_3rd_lib_path="/home/jenkins/opensource"
            ret=$?
            ;;
    esac
fi

if [ $ret -ne 200 ] && [ $ret -ne 0 ]; then
    echo "run ut fail"
    exit 1
fi
if [ $ret -eq 0 ]; then
    coverage_info=$(find "${WORKSPACE}" -type f -name "coverage.info" | head -n1)
    if [ -n "${coverage_info}" ]; then
        mkdir -p "${WORKSPACE}/cov"
        cp -rf ${coverage_info} ${WORKSPACE}/cov/
    fi
    if [ "$coverage_save" = "true" ];then
        echo "ut_process=coverage" >> $ATOMGIT_OUTPUT
    else
        echo "ut_process=ut_cov" >> $ATOMGIT_OUTPUT
    fi
fi
exit 0
