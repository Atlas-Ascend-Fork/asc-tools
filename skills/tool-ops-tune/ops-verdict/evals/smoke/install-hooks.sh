#!/usr/bin/env bash
# -----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# -----------------------------------------------------------------------------------------------------------
# install-hooks.sh — 把 evals + smoke 挂成 pre-push 门禁（本地侧；CI 侧见 .workflow/）
#
# 用法：bash ops-verdict/evals/smoke/install-hooks.sh（在 skills/tool-ops-tune/ 下执行）
set -euo pipefail
# 脚本所在套件目录（三级上溯 = skills/tool-ops-tune）；git 仓库根与 git dir
# 一律以 git 命令解析（检视 #20 修复：直接把套件目录当仓库根会导致
# hooks 写到不存在的 skills/tool-ops-tune/.git/hooks/，set -e 下安装直接失败）
SUITE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
REPO_ROOT="$(git -C "$SUITE_ROOT" rev-parse --show-toplevel)" || {
  echo "ERROR: 不在 git 仓库内（$SUITE_ROOT）——pre-push 门禁需要 git hooks 目录" >&2
  exit 1
}
GIT_DIR="$(git -C "$SUITE_ROOT" rev-parse --absolute-git-dir)"
HOOK="$GIT_DIR/hooks/pre-push"
mkdir -p "$GIT_DIR/hooks"

# 套件在仓内的真实路径（hook 以 git toplevel 为根解析；检视 #18 修复：
# 此前两套路径互相矛盾且都缺 tool-ops-tune 一级 → 每次 push 必被误拦）
SUITE_DIR="skills/tool-ops-tune"
cat > "$HOOK" <<HOOKEOF
#!/usr/bin/env bash
# OpsTune pre-push 门禁（install-hooks.sh 生成；检视 §7b）
set -uo pipefail
REPO_ROOT="\$(git rev-parse --show-toplevel)"
SUITE="\$REPO_ROOT/${SUITE_DIR}"
FAIL=0

EVALS_PY="\$SUITE/ops-verdict/evals/run_evals.py"
SMOKE_SH="\$SUITE/ops-verdict/evals/smoke/run_smoke.sh"
for f in "\$EVALS_PY" "\$SMOKE_SH"; do
  if [ ! -f "\$f" ]; then
    echo "[pre-push] ERROR: 门禁脚本缺失: \$f（套件未完整检出？）" >&2
    exit 1
  fi
done

echo "[pre-push] evals（判定回归 38 用例）…"
python3 "\$EVALS_PY" >/dev/null || {
  echo "[pre-push] FAIL: evals 未全绿 —— 规则/阈值变更未过回归，禁止推送" >&2
  FAIL=1
}

echo "[pre-push] smoke（离线干跑冒烟）…"
bash "\$SMOKE_SH" >/dev/null || {
  echo "[pre-push] FAIL: smoke 冒烟未通过 —— 禁止推送" >&2
  FAIL=1
}

if [[ \$FAIL -ne 0 ]]; then
  exit 1
fi
echo "[pre-push] 门禁全绿，放行"
HOOKEOF
chmod +x "$HOOK"
echo "installed: $HOOK（pre-push 时自动跑 evals + smoke；跳过用 git push --no-verify）"
