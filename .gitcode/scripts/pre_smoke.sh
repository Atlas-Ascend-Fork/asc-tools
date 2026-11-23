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
set +e


log() {
  echo "==== $(date '+%Y%m%d.%H%M%S') : $* ===="
}

rm -rf /root/ascend/log

export ASCEND_GLOBAL_LOG_LEVEL=2
export ASCEND_SLOG_PRINT_TO_STDOUT=0

log "start run test case"

# 下载并安装 asc-tools 包
arm_package="cann-asc-tools_linux-aarch64.run"
wget -nv -O "${arm_package}" "https://ascend-ci.obs.cn-north-4.myhuaweicloud.com/${obs_path}/${arm_package}" 2>/dev/null
if [ ! -f "${arm_package}" ] || [ ! -s "${arm_package}" ]; then
  echo "No custom package found, This PR no need execute smoke."
  rm -f "${arm_package}"
  exit 0
fi

chmod +x "${arm_package}"
source /usr/local/Ascend/cann/set_env.sh
yes "y" | bash "${arm_package}" --full --install-path=/usr/local/Ascend --quiet
source /usr/local/Ascend/cann/set_env.sh
bash ./scripts/run_presmoke.sh 2>&1 | tee -a ./run_test.log

# 打包plog
mkdir -p /root/ascend
slog_name="slog.tar.gz"
tar -zcf slog.tar.gz -C /root/ascend log


# 检查结果
log "checking test results"
date_time=$(date +%Y%m%d.%H%M%S)
if grep -w -e "execute samples success" ./run_test.log; then
  echo "$date_time : run test case success"
else
  echo "$date_time : run test case failed"
  exit 1
fi
