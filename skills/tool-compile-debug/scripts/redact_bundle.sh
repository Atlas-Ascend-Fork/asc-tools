#!/usr/bin/env bash
# ----------------------------------------------------------------------------------------------------------
# 用途：对外分享前对产物做本地脱敏（包装 scripts/run.py redact）。
# 使用方法：bash redact_bundle.sh <输入文件> <输出文件> [--stats <统计JSON>]
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
# Package wrapper for local bundle redaction.
# Redaction is irreversible by default. Supported host: Linux with Linux Python.
set -euo pipefail

if [ "$(uname -s)" != "Linux" ]; then
  echo "Unsupported platform: run this skill on Linux with Linux Python (the toolkit targets a supported CANN environment >=9.0.0)." >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# The installed directory is a read-only distribution surface. Do not leave
# __pycache__ or .pyc files in it.
export PYTHONDONTWRITEBYTECODE=1

python_cmd="${ASCENDC_DOCTOR_PYTHON:-}"
# Probe that the selected interpreter is a supported Linux Python.
_py_ok() {
  "$1" -c "import sys; import yaml; sys.exit(0 if sys.platform.startswith('linux') and sys.version_info >= (3, 9) else 1)" >/dev/null 2>&1
}

if [ -z "$python_cmd" ]; then
  for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 && _py_ok "$candidate"; then
      python_cmd="$candidate"
      break
    fi
  done
elif ! _py_ok "$python_cmd"; then
  echo "ERROR: ASCENDC_DOCTOR_PYTHON='$python_cmd' is not a runnable Python interpreter." >&2
  echo "  Point it to a working Linux Python 3.9+ with PyYAML." >&2
  exit 2
fi

if [ -z "$python_cmd" ]; then
  echo "ERROR: no usable Python 3 interpreter found (probed: \$ASCENDC_DOCTOR_PYTHON, python3, python)." >&2
  echo "  Install Linux Python 3.9+ with PyYAML, or set ASCENDC_DOCTOR_PYTHON to a working interpreter." >&2
  exit 2
fi

exec "$python_cmd" "$script_dir/run.py" redact "$@"
