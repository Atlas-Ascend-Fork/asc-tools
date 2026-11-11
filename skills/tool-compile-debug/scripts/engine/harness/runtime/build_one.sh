#!/usr/bin/env bash
# ----------------------------------------------------------------------------------------------------------
# 用途：在独立临时副本中按原构建配方执行一次编译，供真实编译验证取证。
# Usage: bash build_one.sh CASE_DIR [SOC_VERSION] [--target NAME] [--cmake-arg ARG] [-- build.sh ARGS...]
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
set -uo pipefail

CASE_DIR="${1:?usage: build_one.sh CASE_DIR [SOC_VERSION] [--target NAME] [--cmake-arg ARG] [-- build.sh ARGS...]}"
shift
REQUESTED_SOC_VERSION="${SOC_VERSION:-}"
if [ "$#" -gt 0 ] && [[ "$1" != --* ]]; then
  REQUESTED_SOC_VERSION="$1"
  shift
fi
BUILD_SCRIPT_ARGS=()
EXTRA_CMAKE_ARGS=()
BUILD_TARGET=""
BUILD_TARGET_SET=0
set_build_target() {
  if [ "$BUILD_TARGET_SET" -ne 0 ]; then
    echo "--target may be specified only once. Preserve a multi-target recipe through the original build.sh arguments, or verify each target separately and keep every report." >&2
    exit 64
  fi
  if [ -z "${1//[[:space:]]/}" ] || [[ "$1" == -* ]]; then
    echo "--target requires one nonempty target name, not an option" >&2
    exit 64
  fi
  BUILD_TARGET="$1"
  BUILD_TARGET_SET=1
}
while [ "$#" -gt 0 ]; do
  case "$1" in
    --) shift; BUILD_SCRIPT_ARGS=("$@"); break ;;
    --target) [ "$#" -ge 2 ] || { echo "--target needs a value" >&2; exit 64; }; set_build_target "$2"; shift 2 ;;
    --target=*) set_build_target "${1#--target=}"; shift ;;
    --cmake-arg) [ "$#" -ge 2 ] || { echo "--cmake-arg needs one argv value" >&2; exit 64; }; EXTRA_CMAKE_ARGS+=("$2"); shift 2 ;;
    --cmake-arg=*) EXTRA_CMAKE_ARGS+=("${1#--cmake-arg=}"); shift ;;
    *) echo "Unknown build_one argument: $1" >&2; exit 64 ;;
  esac
done
SOC_VERSION="$REQUESTED_SOC_VERSION"
if [ "$SOC_VERSION" = "Ascend910B" ]; then
  detected_soc="$(npu-smi info 2>/dev/null | awk '/910B[0-9A-Za-z-]*/ {for (i=1;i<=NF;i++) if ($i ~ /^910B/) {print "Ascend"$i; exit}}' || true)"
  SOC_VERSION="${detected_soc:-Ascend910B}"
fi
# An explicit SoC determines its target even on a different physical host. With
# no SoC, preserve the caller's explicit architecture or the project's default.
# Never silently turn a 950 request into the old unconditional dav-2201 default.
ASC_ARCH="${CMAKE_ASC_ARCHITECTURES:-}"
case "$SOC_VERSION" in
  Ascend950*|ascend950*) ASC_ARCH="dav-3510" ;;
  Ascend910B*|ascend910b*) ASC_ARCH="dav-2201" ;;
esac
CASE_DIR="$(cd "$CASE_DIR" && pwd)"
BUILD_DIR="${CASE_DIR}/build"
LOG_DIR="${CASE_DIR}/logs"
LOG_FILE="${LOG_DIR}/build.log"

mkdir -p "$BUILD_DIR" "$LOG_DIR"

status=0
(
  set -euo pipefail
  cd "$CASE_DIR"
  echo "=== CASE_DIR=$CASE_DIR ==="
  echo "=== SOC_VERSION=$SOC_VERSION ==="
  echo "=== REQUESTED_SOC_VERSION=$REQUESTED_SOC_VERSION ==="
  echo "=== CMAKE_ASC_ARCHITECTURES=${ASC_ARCH:-project-default} ==="
  echo "=== DATE=$(date -Is) ==="

  [ -z "$SOC_VERSION" ] || export SOC_VERSION
  [ -z "$ASC_ARCH" ] || export CMAKE_ASC_ARCHITECTURES="$ASC_ARCH"
  CANN_ROOT="${ASCEND_CANN_PACKAGE_PATH:-${ASCEND_TOOLKIT_HOME:-/usr/local/Ascend/ascend-toolkit/latest}}"
  if [ ! -d "$CANN_ROOT" ]; then
    echo "Selected CANN toolkit directory does not exist: $CANN_ROOT" >&2
    exit 86
  fi
  CANN_ROOT="$(cd "$CANN_ROOT" && pwd -P)"
  export ASCEND_CANN_PACKAGE_PATH="$CANN_ROOT"
  export ASCEND_TOOLKIT_HOME="$CANN_ROOT"
  # Read the toolkit version from standard metadata so verification results carry
  # the version evidence that was available during the build.
  ENGINE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  _CANN_VER="$(PYTHONPATH="$ENGINE_ROOT${PYTHONPATH:+:$PYTHONPATH}" python3 -c \
    'import os; from pathlib import Path; from harness.engine.env_probe import read_cann_metadata; print(read_cann_metadata(Path(os.environ["ASCEND_CANN_PACKAGE_PATH"]))["version"])')"
  echo "=== CANN_VERSION=${_CANN_VER:-unknown} ==="
  ASC_CMAKE_DIR="${ASC_DIR:-}"
  if [ -z "$ASC_CMAKE_DIR" ]; then
    for d in \
      "$CANN_ROOT/lib64/cmake" \
      "$CANN_ROOT/$(uname -m)-linux/lib64/cmake" \
      "$CANN_ROOT/$(uname -m)-linux/tikcpp/ascendc_kernel_cmake"; do
      if [ -f "$d/asc-config.cmake" ] || [ -f "$d/ASCConfig.cmake" ]; then
        ASC_CMAKE_DIR="$d"
        break
      fi
    done
  fi
  export ASC_DIR="$ASC_CMAKE_DIR"
  export CMAKE_PREFIX_PATH="$ASC_CMAKE_DIR:$CANN_ROOT:$CANN_ROOT/lib64/cmake:$CANN_ROOT/cmake${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
  # Keep the project/cache build type unless the caller explicitly supplies one.
  CMAKE_ARGS=(
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  )
  [ -z "$SOC_VERSION" ] || CMAKE_ARGS+=(-DSOC_VERSION="$SOC_VERSION")
  [ -z "$ASC_ARCH" ] || CMAKE_ARGS+=(-DCMAKE_ASC_ARCHITECTURES="$ASC_ARCH")
  if [ -n "$ASC_CMAKE_DIR" ]; then
    CMAKE_ARGS+=(-DASC_DIR="$ASC_CMAKE_DIR")
  fi
  # 可选：调用方经 ASCENDC_PYTHON3_HINT 指定带 Development 头的 Python3，供模板里
  # find_package(Python3 COMPONENTS ... Development) 用（某些机的默认 /usr/bin/python3 无 dev 头，
  # 会让 torch-extension 目标在 configure 期失败、build 走不到 kernel 编译）。仅在显式设置时注入。
  if [ -n "${ASCENDC_PYTHON3_HINT:-}" ] && [ -x "${ASCENDC_PYTHON3_HINT}" ]; then
    CMAKE_ARGS+=(-DPython3_EXECUTABLE="${ASCENDC_PYTHON3_HINT}")
  fi

  if [ -f build.sh ]; then
    if [ -n "$BUILD_TARGET" ] || [ "${#EXTRA_CMAKE_ARGS[@]}" -gt 0 ]; then
      echo "Project has build.sh; pass its original arguments after -- (do not replace its recipe)" >&2
      exit 64
    fi
    printf '=== BUILD_ARGV:'; printf ' %q' bash build.sh "${BUILD_SCRIPT_ARGS[@]}"; printf ' ===\n'
    bash build.sh "${BUILD_SCRIPT_ARGS[@]}" 2>&1
  elif [ -f CMakeLists.txt ]; then
    if [ "${#BUILD_SCRIPT_ARGS[@]}" -gt 0 ]; then
      echo "Arguments after -- require an existing build.sh" >&2
      exit 64
    fi
    # Request the configured target/source graph independently of compiler results.
    mkdir -p build/.cmake/api/v1/query
    : > build/.cmake/api/v1/query/codemodel-v2
    cmake -S . -B build "${EXTRA_CMAKE_ARGS[@]}" "${CMAKE_ARGS[@]}" 2>&1
    BUILD_COMMAND=(cmake --build build --verbose -j"${BUILD_JOBS:-8}")
    [ -z "$BUILD_TARGET" ] || BUILD_COMMAND+=(--target "$BUILD_TARGET")
    "${BUILD_COMMAND[@]}" 2>&1
  else
    echo "No build.sh or CMakeLists.txt found" >&2
    exit 88
  fi
) 2>&1 | tee "$LOG_FILE" || status=$?

for f in \
  "$BUILD_DIR/compile_commands.json" \
  "$BUILD_DIR/CMakeCache.txt" \
  "$BUILD_DIR/CMakeFiles/CMakeOutput.log" \
  "$BUILD_DIR/CMakeFiles/CMakeError.log"; do
  [ -f "$f" ] && cp "$f" "$LOG_DIR/" || true
done

exit "$status"
