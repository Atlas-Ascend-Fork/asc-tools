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

set -e

SUPPORTED_SHORT_OPTS=("h" "j" "t" "p")
SUPPORTED_LONG_OPTS=(
  "help" "cov" "cache" "pkg" "msot" "asan" "make_clean" "cann_3rd_lib_path" "test" "cann_path" "build-type" "pkg-type" "cpp_utest" "python_utest" "extra-cmake-args"
)

CURRENT_DIR=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
BUILD_DIR=${CURRENT_DIR}/build
OUTPUT_DIR=${CURRENT_DIR}/build_out
USER_ID=$(id -u)
CPU_NUM=$(($(cat /proc/cpuinfo | grep "^processor" | wc -l)))
THREAD_NUM=${CPU_NUM}
CUSTOM_OPTION="-DCMAKE_INSTALL_PREFIX=${OUTPUT_DIR} -DBUILD_OPEN_PROJECT=ON"
CANN_3RD_LIB_PATH=${CURRENT_DIR}/third_party
BUILD_TYPE="Release"
PACKAGE_TYPE="run"

dotted_line="----------------------------------------------------------------"

usage() {
  local specific_help="$1"

  if [[ -n "$specific_help" ]]; then
    case "$specific_help" in
      package)
        echo "Package Build Options:"
        echo $dotted_line
        echo "    --pkg                Compile asc-tools package"
        echo "    --pkg --msot         Compile msot package"
        echo "    -p, --cann_path      Set the cann package installation directory, eg: /usr/local/Ascend/latest"
        echo "    -j                   Compile thread nums, default is 16, eg: -j 8"
        echo "    --cann_3rd_lib_path  Set the path for third-party library dependencies, eg: ./build"
        echo "    --asan               Enable ASAN (address Sanitizer)"
        echo "    --pkg-type=<TYPE>    Specify package type (TYPE options: run/rpm/deb), Default: run"
        echo $dotted_line
        echo "Examples:"
        echo "    bash build.sh --pkg                    # Build asc-tools package"
        echo "    bash build.sh --pkg --pkg-type=rpm     # Build asc-tools rpm package"
        echo "    bash build.sh --pkg --msot             # Build msot package"
        echo "    bash build.sh --pkg -j 8"
        echo "    bash build.sh --pkg --asan -j 32"
        return
        ;;
      test)
        echo "Test Options:"
        echo $dotted_line
        echo "    -t, --test           Build and run all unit tests"
        echo "    -p, --cann_path      Set the cann package installation directory, eg: /usr/local/Ascend/latest"
        echo "    -j                   Compile thread nums, default is 16, eg: -j 8"
        echo "    --cpp_utest             Build and run the cpp part of unit tests"
        echo "    --python_utest          Build and run the python part of unit tests"
        echo "    --cann_3rd_lib_path  Set the path for third-party library dependencies, eg: ./build"
        echo "    --cov                Enable code coverage for unit tests"
        echo "    --asan               Enable ASAN (address Sanitizer)"
        echo $dotted_line
        echo "Examples:"
        echo "    bash build.sh -t --cov"
        echo "    bash build.sh --test_part --asan -j 32"
        return
        ;;
      clean)
        echo "Clean Options:"
        echo $dotted_line
        echo "    --make_clean         Clean build artifacts"
        echo $dotted_line
        echo "Examples:"
        echo "    bash build.sh -t --make_clean"
        return
        ;;
    esac
  fi

  echo "build script for asc-tools repository"
  echo "Usage: bash build.sh [OPTION]..."
  echo ""
  echo "    The following are all supported arguments:"
  echo $dotted_line
  echo "    -h, --help           Display help information"
  echo "    -j                   Compile thread nums, default is 16, eg: -j 8"
  echo "    -t, --test           Build and run all unit tests"
  echo "    -p, --cann_path      Set the cann package installation directory, eg: /usr/local/Ascend/latest"
  echo "    --cpp_utest           Build and run the cpp part of unit tests"
  echo "    --python_utest        Build and run the python part of unit tests"
  echo "    --pkg                Compile asc-tools package"
  echo "    --pkg --msot         Compile msot package"
  echo "    --cann_3rd_lib_path  Set the path for third-party library dependencies, eg: ./build"
  echo "    --cov                Enable code coverage for unit tests"
  echo "    --asan               Enable ASAN (address Sanitizer)"
  echo "    --make_clean         Clean build artifacts"
  echo "    --build-type=<TYPE>"
  echo "                         Specify build type (TYPE options: Release/Debug), Default:Release"
  echo "    --pkg-type=<TYPE>"
  echo "                         Specify package type (TYPE options: run/rpm/deb), Default:run"
}

function log() {
  local current_time=`date +"%Y-%m-%d %H:%M:%S"`
  echo "[$current_time] "$1
}

parse_cmake_extra_args() {
    echo "Parse cmake extra args."
    # para check
    local args_str="$1"
    if [[ -z "$args_str" ]]; then
        echo "The parsed parameter string is empty."
        return 0
    fi

    IFS=',' read -ra kv_pairs <<< "$args_str"

    for kv_pair in "${kv_pairs[@]}"; do
        if [[ -z "$kv_pair" ]]; then
            continue
        fi

        local key="${kv_pair%%=*}"
        local value="${kv_pair#*=}"

        case "$key" in
            "ENABLE_MOD_EXT")
                ENABLE_MOD_EXT="$value"
                echo "Set ENABLE_MOD_EXT to ${ENABLE_MOD_EXT}."
                ;;
            *)
                echo "invalid parameter key: $key"
                ;;
        esac
    done
}

function copy_deps_file() {
  if ls ${BUILD_DIR}/simulator*.tar.gz 1> /dev/null 2>&1; then
    cp -r ${BUILD_DIR}/simulator*.tar.gz ${CANN_3RD_LIB_PATH}
  fi

  if ls ${BUILD_DIR}/cann-asc-tools-cpudebug-deps*.tar.gz 1> /dev/null 2>&1; then
    cp -r ${BUILD_DIR}/cann-asc-tools-cpudebug-deps*.tar.gz ${CANN_3RD_LIB_PATH}
  fi

  local dep_file
  for dep_file in "${BUILD_DIR}"/cann-asc-tools-deps_*.tar.gz "${BUILD_DIR}"/cann_asc_tools_deps_*_pkg_compile.rar; do
    if [[ -f "${dep_file}" ]]; then
      cp "${dep_file}" "${CANN_3RD_LIB_PATH}/"
    fi
  done
}

function clean()
{
  if [ -n "${BUILD_DIR}" ];then
    rm -rf ${BUILD_DIR}
  fi

  if [ -n "${OUTPUT_DIR}" ];then
    rm -rf ${OUTPUT_DIR}
  fi

  mkdir -p ${BUILD_DIR} ${OUTPUT_DIR}
}

check_option_validity() {
  local arg="$1"

  if [[ "$arg" =~ "=" ]]; then
    arg="${arg%%=*}"
  fi

  if [[ "$arg" =~ ^-[^-] ]]; then
    if [[ $arg =~ ^-j[0-9]+$ ]]; then
      return 0
    fi

    if [[ ! " ${SUPPORTED_SHORT_OPTS[@]} " =~ " ${arg:1} " ]]; then
      log "[ERROR] Invalid short option: ${arg}"
      return 1
    fi
  fi

  if [[ "$arg" =~ ^-- ]]; then
    if [[ ! " ${SUPPORTED_LONG_OPTS[@]} " =~ " ${arg:2} " ]]; then
      log "[ERROR] Invalid long option: ${arg}"
      return 1
    fi
  fi
  return 0
}

check_help_combinations() {
  local args=("$@")
  local has_test=false
  local has_cov=false
  local has_pkg=false
  local has_msot=false

  for arg in "${arg[@]}"; do
    case "$arg" in
      -t|--test) has_test=true ;;
      --cpp_utest) test_part="cpp_utest" ;;
      --python_utest) test_part="python_utest" ;;
      --cov) has_cov=true ;;
      --pkg) has_pkg=true ;;
      --msot) has_msot=true ;;
      -h|--help) ;;
    esac
  done

  if [[ ("$has_test" == "true") && "$has_pkg" == "true" ]]; then
    log "[ERROR] --pkg cannot be used with test(-t, --test)."
    return 1
  fi

  if [[ ("$has_test" == "true") && "$has_msot" == "true" ]]; then
    log "[ERROR] --msot cannot be used with test(-t, --test)."
    return 1
  fi

  return 0
}

check_param_with_help() {
  for arg in "$@"; do
    if [[ "$arg" =~ ^- ]]; then
      if ! check_option_validity "$arg"; then
        log "[INFO] Use 'bash build.sh --help' for more information."
        exit 1
      fi
    fi
  done

  seen=()
  for arg in "$@"; do
    arg="${arg%%=*}"
    if [[ " ${seen[@]} " =~ " $arg " ]]; then
      log "[ERROR] $arg can only be input one."
      exit 1
    fi
    seen+=("$arg")
  done

  for arg in "$@"; do
    if [[ "$arg" == "--help" || "$arg" == "-h" ]]; then
      check_help_combinations "$@"
      local comb_result=$?
      if [ $comb_result -eq 1 ]; then
        exit 1
      fi
      SHOW_HELP="general"

      for prev_arg in "$@"; do
        case "$prev_arg" in
          --pkg) SHOW_HELP="package" ;;
          --msot) SHOW_HELP="package" ;;
          -t|--test) SHOW_HELP="test" ;;
          --cpp_utest) SHOW_HELP="utest" ;;
          --python_utest) SHOW_HELP="utest" ;;
          --make_clean) SHOW_HELP="clean" ;;
        esac
      done

      usage "$SHOW_HELP"
      exit 0
    fi
  done
}

function cmake_config()
{
  local extra_option="$1"
  log "Info: cmake config ${CUSTOM_OPTION} ${extra_option} ."
  cmake .. ${CUSTOM_OPTION} ${extra_option}
}

function build()
{
  local target="$1"
  cmake --build . --target ${target} -j ${THREAD_NUM}
}

function build_package(){
  cmake_config
  build package
}

function build_test() {
  cmake_config
  build all
}

function set_env()
{
  if [ "${USER_ID}" != "0" ]; then
    DEFAULT_TOOLKIT_INSTALL_DIR="${HOME}/Ascend/ascend-toolkit/latest"
    DEFAULT_INSTALL_DIR="${HOME}/Ascend/latest"
  else
    DEFAULT_TOOLKIT_INSTALL_DIR="/usr/local/Ascend/ascend-toolkit/latest"
    DEFAULT_INSTALL_DIR="/usr/local/Ascend/latest"
  fi

  if [ -n "${cann_path}" ];then
    ASCEND_CANN_PACKAGE_PATH=${cann_path}
  elif [ -n "${ASCEND_HOME_PATH}" ];then
    ASCEND_CANN_PACKAGE_PATH=${ASCEND_HOME_PATH}
  elif [ -n "${ASCEND_OPP_PATH}" ];then
    ASCEND_CANN_PACKAGE_PATH=$(dirname ${ASCEND_OPP_PATH})
  elif [ -d "${DEFAULT_TOOLKIT_INSTALL_DIR}" ];then
    ASCEND_CANN_PACKAGE_PATH=${DEFAULT_TOOLKIT_INSTALL_DIR}
  elif [ -d "${DEFAULT_INSTALL_DIR}" ];then
    ASCEND_CANN_PACKAGE_PATH=${DEFAULT_INSTALL_DIR}
  else
    log "Error: Please set the cann package installation directory through parameter -p|--cann_path."
    exit 1
  fi

  source $ASCEND_CANN_PACKAGE_PATH/bin/setenv.bash || echo "0"
}

check_param_j() {
  if [[ ! $THREAD_NUM =~ ^-?[0-9]+$ ]]; then
   log "[ERROR] -j only support positive integers."
   exit 1
  fi

  if [[ "$THREAD_NUM" -gt "$CPU_NUM" ]]; then
    log "[WARNING] compile thread num:$THREAD_NUM over core num:$CPU_NUM, adjust to core num."
    THREAD_NUM=$CPU_NUM
  fi
}

check_pkg_type() {
  local pkg_type="$1"
  if [[ "${pkg_type}" != "run" && "${pkg_type}" != "rpm" && "${pkg_type}" != "deb" ]]; then
    log "[ERROR] Invalid value ${pkg_type} for option --pkg-type"
    usage
    exit 1
  fi
}

check_param_clean() {
  if [[ "$#" -gt 1 || ( "$#" -eq 2 && $has_h == "true" ) ]]; then
    log "[ERROR] --make_clean must be used separately."
    exit 1
  fi
}

check_param_test_pkg() {
  if [[ "$TEST" == "all" && "$PKG" == "true" ]]; then
    log "[ERROR] --pkg cannot be used with test(-t, --test)."
    exit 1
  fi
  if [[ "$TEST" == "all" && "$IS_BUILD" == "true" ]]; then
    log "[ERROR] --build-type cannot be used with test(-t, --test)."
    exit 1
  fi
  if [[ "$TEST" == "all" && "$MSOT" == "true" ]]; then
    log "[ERROR] --msot cannot be used with test(-t, --test)."
    exit 1
  fi
}

get_absolute_path() {
  CANN_3RD_LIB_PATH=$(cd ${CANN_3RD_LIB_PATH} && pwd -P)
}

check_param_test_part() {
  if [[ "$TEST" == "all" && -n "$TEST_PART" ]]; then
    log "[ERROR] --$TEST_PART cannot be used with test(-t, --test)."
    exit 1
  fi
}

set_options() {
  while [[ $# -gt 0 ]]; do
    case $1 in
    -h|--help)
      has_h="true"
      usage
      exit 0
      ;;
    --ccache)
      CCACHE_PROGRAM="$2"
      shift 2
      ;;
    -p=*|--cann_path=*)
      cann_path="${1#*=}"
      shift
      ;;
    -p|--cann_path)
      cann_path="$2"
      shift 2
      ;;
    -t|--test)
      TEST="all"
      check_param_test_pkg
      shift
      ;;
    --cpp_utest)
      TEST_PART="cpp_utest"
      check_param_test_part
      shift
      ;;
    --python_utest)
      TEST_PART="python_utest"
      check_param_test_part
      shift
      ;;
    --extra-cmake-args=*)
      local cmake_args="${1#*=}"
      parse_cmake_extra_args "${cmake_args}"
      shift
      ;;
    --asan)
      ASAN="true"
      shift
      ;;
    --cov)
      COV="true"
      shift
      ;;
    --pkg)
      PKG="true"
      check_param_test_pkg
      shift
      ;;
    --msot)
      MSOT="true"
      check_param_test_pkg
      shift
      ;;
    --cann_3rd_lib_path=*)
      CANN_3RD_LIB_PATH="${1#*=}"
      get_absolute_path
      shift
      ;;
    --cann_3rd_lib_path)
      CANN_3RD_LIB_PATH="$2"
      get_absolute_path
      shift 2
      ;;
    --make_clean)
      MAKE_CLEAN="true"
      check_param_clean
      clean
      exit 0
      ;;
    -j*)
      THREAD_NUM="${1#-j}"
      check_param_j
      shift
      ;;
    -j=*)
      THREAD_NUM="${1#*=}"
      check_param_j
      shift
      ;;
    -j)
      THREAD_NUM="$2"
      check_param_j
      shift 2
      ;;
    --build-type=*)
      IS_BUILD="true"
      BUILD_TYPE="${1#*=}"
      check_param_test_pkg
      shift
      ;;
    --build-type)
      IS_BUILD="true"
      BUILD_TYPE="$2"
      check_param_test_pkg
      shift 2
      ;;
    --pkg-type=*)
      PACKAGE_TYPE="${1#*=}"
      check_pkg_type "${PACKAGE_TYPE}"
      shift
      ;;
    --pkg-type)
      PACKAGE_TYPE="$2"
      check_pkg_type "${PACKAGE_TYPE}"
      shift 2
      ;;
    *)
      log "[ERROR] Undefined option: $1"
      usage
      break
      ;;
    esac
  done
}

function build_test_part() {
  if [[ "$TEST_PART" == "cpp_utest" ]]; then
    CUSTOM_OPTION="${CUSTOM_OPTION} -DTEST_MOD=cpp"
  elif [[ "$TEST_PART" == "python_utest" ]]; then
    CUSTOM_OPTION="${CUSTOM_OPTION} -DTEST_MOD=python"
  fi
  build_test
  return 0
}

# msot子仓列表
MSOT_SUBMODULES=("msopgen" "msdebug" "mskpp" "mskl" "msopprof" "mssanitizer")

# 各子仓的三方库依赖配置
# 格式: "子仓名:thirdparty目录名:三方库名1,三方库名2,..."
# 支持源名->目标名映射格式，例如：libc_sec->securec
declare -A SUBMODULE_THIRDPARTY_DEPS
SUBMODULE_THIRDPARTY_DEPS["msot"]="thirdparty:makeself"
SUBMODULE_THIRDPARTY_DEPS["msdebug"]="third-party:makeself,libedit,ncurses"
SUBMODULE_THIRDPARTY_DEPS["mskpp"]=""  # mskpp无三方库依赖
SUBMODULE_THIRDPARTY_DEPS["msopprof"]="thirdparty:json,libc_sec->securec,makeself,llvm-19->llvm-project"
SUBMODULE_THIRDPARTY_DEPS["msopgen"]="thirdparty:asc-tools"
SUBMODULE_THIRDPARTY_DEPS["mssanitizer"]="thirdparty:json,libc_sec->securec,makeself,llvm-19->llvm-project"

function detect_msot_zone() {
  local mindstudio_path="${CANN_3RD_LIB_PATH}/../mindstudio"
  if [[ -d "${mindstudio_path}" ]]; then
    MSOT_MINDSTUDIO_PATH=$(cd "${mindstudio_path}" && pwd -P)
  fi
}

# 创建单个软链
function create_symlink() {
  local src_path="$1"
  local dst_path="$2"

  if [[ ! -e "${src_path}" && ! -L "${src_path}" ]]; then
    log "[WARNING] Source not found: ${src_path}"
    return 1
  fi

  if [[ -e "${dst_path}" || -L "${dst_path}" ]]; then
    log "[INFO] Symlink already exists: ${dst_path}"
    return 0
  fi

  ln -s "${src_path}" "${dst_path}"
  log "[INFO] Created symlink: ${dst_path} -> ${src_path}"
  return 0
}

# 创建msot子仓软链
function create_msot_submodule_symlinks() {
  local msot_path="$1"
  local mindstudio_path="$2"

  log "Creating submodule symlinks for existing msot..."

  # 创建子仓软链
  for submodule in "${MSOT_SUBMODULES[@]}"; do
    local src_path="${mindstudio_path}/${submodule}"
    local dst_path="${msot_path}/${submodule}"
    create_symlink "${src_path}" "${dst_path}"
  done
}

# 创建msopscommon软链（msopprof和mssanitizer依赖）
function create_msopscommon_symlinks() {
  local msot_path="$1"
  local mindstudio_path="$2"

  log "Creating msopscommon symlinks..."

  local msopcom_src="${mindstudio_path}/msopcom"

  # msopprof/msopscommon -> mindstudio/msopcom
  local msopprof_path="${msot_path}/msopprof"
  if [[ -d "${msopprof_path}" ]]; then
    create_symlink "${msopcom_src}" "${msopprof_path}/msopscommon"
  fi

  # mssanitizer/msopscommon -> mindstudio/msopcom
  local mssanitizer_path="${msot_path}/mssanitizer"
  if [[ -d "${mssanitizer_path}" ]]; then
    create_symlink "${msopcom_src}" "${mssanitizer_path}/msopscommon"
  fi

  # 为 msopcom 创建三方依赖软链（json等）
  create_msopcom_thirdparty_symlinks "${msot_path}" "${mindstudio_path}"
}

# 为 msopcom 创建三方依赖软链
function create_msopcom_thirdparty_symlinks() {
  local msot_path="$1"
  local mindstudio_path="$2"

  log "Creating msopcom third-party symlinks..."

  local msopcom_src="${mindstudio_path}/msopcom"

  if [[ ! -d "${msopcom_src}" ]]; then
    log "[WARNING] msopcom not found: ${msopcom_src}"
    return
  fi

  # 为每个依赖 msopcom 的子仓创建三方库软链
  for parent in "msopprof" "mssanitizer"; do
    local parent_path="${msot_path}/${parent}"
    local msopscommon_path="${parent_path}/msopscommon"

    if [[ ! -d "${msopscommon_path}" ]]; then
      continue
    fi

    local thirdparty_dst="${msopscommon_path}/thirdparty"
    create_symlink "${CANN_3RD_LIB_PATH}/json" "${thirdparty_dst}/json"
  done
}

# 创建三方库软链
function create_thirdparty_symlinks() {
  local msot_path="$1"

  log "Creating third-party library symlinks..."

  # 三方库源路径（CANN_3RD_LIB_PATH）
  local thirdparty_src_base="${CANN_3RD_LIB_PATH}"
  # asc-tools 特殊路径（与mindstudio同级）
  local asc_tools_src="${CANN_3RD_LIB_PATH}/../asc/asc-tools"
  local securec_src="${CANN_3RD_LIB_PATH}/../abl/libc_sec"

  for submodule in "${MSOT_SUBMODULES[@]}" "msot"; do
    local config="${SUBMODULE_THIRDPARTY_DEPS[$submodule]}"
    if [[ -z "${config}" ]]; then
      continue
    fi

    # 解析配置: "thirdparty:lib1,lib2,..."
    local thirdparty_dir="${config%%:*}"
    local libs="${config#*:}"

    # 如果没有依赖库，跳过
    if [[ -z "${libs}" ]]; then
      continue
    fi

    local submodule_path
    if [[ "${submodule}" == "msot" ]]; then
      submodule_path="${msot_path}"
    else
      submodule_path="${msot_path}/${submodule}"
    fi

    # 如果子仓目录不存在，跳过
    if [[ ! -d "${submodule_path}" ]]; then
      log "[WARNING] Submodule directory not found: ${submodule_path}"
      continue
    fi

    # 创建 thirdparty 目录（如果不存在）
    local thirdparty_dst="${submodule_path}/${thirdparty_dir}"
    mkdir -p "${thirdparty_dst}"

    # 为每个三方库创建软链
    IFS=',' read -ra lib_array <<< "${libs}"
    for lib in "${lib_array[@]}"; do
      local lib_src lib_dst
      local lib_src_name lib_dst_name

      # 解析 "源名->目标名" 格式
      if [[ "${lib}" == *"->"* ]]; then
        lib_src_name="${lib%%->*}"
        lib_dst_name="${lib##*->}"
      else
        lib_src_name="${lib}"
        lib_dst_name="${lib}"
      fi

      # makeself 特殊处理：复制 tar.gz 和 patch 到子仓，让 CMake 正常解压
      if [[ "${lib_dst_name}" == "makeself" ]]; then
        local makeself_src="${thirdparty_src_base}/${lib_src_name}"
        local makeself_dst="${thirdparty_dst}/${lib_dst_name}"

        if [[ -d "${makeself_src}" ]]; then
          # 确保目标目录存在
          mkdir -p "${makeself_dst}"

          # 检查源目录是否已有 tar 包和 patch 文件
          if ls ${makeself_src}/makeself-*.tar.gz 1>/dev/null 2>&1 && ls ${makeself_src}/makeself-*.patch 1>/dev/null 2>&1; then
            # 已有 tar.gz 和 patch，直接复制到目标目录
            log "Copying existing makeself tar and patch to ${makeself_dst}..."
            local existing_tar=$(ls ${makeself_src}/makeself-*.tar.gz 2>/dev/null | head -1)
            local existing_patch=$(ls ${makeself_src}/makeself-*.patch 2>/dev/null | head -1)
            cp -f "${existing_tar}" "${makeself_dst}/"
            cp -f "${existing_patch}" "${makeself_dst}/"
            log "[INFO] Copied: ${existing_tar} and ${existing_patch}"
          else
            # 在源目录创建 tar 包和 patch 文件
            log "Creating makeself tar and patch in ${makeself_src}..."

            # 创建 tar 包（带外层目录 makeself-custom/）
            local tar_name="makeself-custom.tar.gz"
            local patch_name="makeself-custom.patch"
            local temp_tar=$(mktemp)
            local temp_dir=$(mktemp -d)
            local extract_dir=$(mktemp -d)
            ln -s "${makeself_src}" "${temp_dir}/makeself-custom"
            tar czhf "${temp_tar}" -C "${temp_dir}" makeself-custom
            rm -rf "${temp_dir}"
            mv "${temp_tar}" "${makeself_src}/${tar_name}"
            log "[INFO] Created ${makeself_src}/${tar_name}"

            # 创建固定格式的 patch 文件
            # makeself.sh 文件内容固定，使用硬编码的空操作 patch
            cat > "${makeself_src}/${patch_name}" << 'PATCH_EOF'
diff --git a/makeself.sh b/makeself.sh
index aa55dc0..302f96a 100755
--- a/makeself.sh
+++ b/makeself.sh
@@ -1,5 +1,6 @@
 #!/bin/sh
 #
+#
 # Makeself version 2.5.x
 #  by Stephane Peter <megastep@megastep.org>
 #

PATCH_EOF
            log "[INFO] Created ${makeself_src}/${patch_name}"

            # 复制 tar.gz 和 patch 到目标目录
            cp -f "${makeself_src}/${tar_name}" "${makeself_dst}/"
            cp -f "${makeself_src}/${patch_name}" "${makeself_dst}/"
            log "[INFO] Copied to ${makeself_dst}/"
          fi
        else
          log "[WARNING] makeself source not found: ${makeself_src}"
        fi
        continue
      fi

      # 确定源路径
      if [[ "${lib_dst_name}" == "asc-tools" ]]; then
        # asc-tools 特殊处理：指向asc-tools路径
        lib_src="${asc_tools_src}"
      elif [[ "${lib_dst_name}" == "securec" ]]; then
        # securec 特殊处理：路径为 cann_3rd_lib_path/../abl/libc_sec
        lib_src="${securec_src}"
      else
        lib_src="${thirdparty_src_base}/${lib_src_name}"
      fi

      lib_dst="${thirdparty_dst}/${lib_dst_name}"
      create_symlink "${lib_src}" "${lib_dst}"
    done
  done
}

# 准备msot submodule（自动配置）
function prepare_msot_submodule() {
  local msot_path="$1"
  local msot_url="https://gitcode.com/Ascend/msot.git"

  # 情况1：msot目录已存在
  if [[ -d "${msot_path}" ]]; then
    # 检查是否是git submodule
    if [[ -f "${msot_path}/.git" ]] || git config --file .gitmodules --get submodule.msot.url >/dev/null 2>&1; then
      log "msot submodule already configured"
      return 0
    fi

    # 检查是否是软链
    if [[ -L "${msot_path}" ]]; then
      log "Using existing msot symlink: ${msot_path}"
      return 0
    fi

    # 已存在普通目录，检查是否是git仓库
    if [[ -d "${msot_path}/.git" ]]; then
      log "Using existing msot git repository: ${msot_path}"
      return 0
    fi

    log "[WARNING] ${msot_path} exists but is not a valid git repository or submodule"
    return 1
  fi

  # 情况2：msot未配置，尝试自动添加submodule
  log "msot not found, adding as git submodule..."

  # 检查是否已经在.gitmodules中配置
  if git config --file .gitmodules --get submodule.msot.url >/dev/null 2>&1; then
    log "msot already in .gitmodules, initializing..."
    git submodule update --init --no-fetch msot
    return $?
  fi

  # 自动添加submodule（不递归）
  log "Adding msot submodule from ${msot_url}..."
  git submodule add --depth 1 "${msot_url}" msot
  if [[ $? -ne 0 ]]; then
    log "[ERROR] Failed to add msot submodule"
    return 1
  fi

  log "msot submodule added successfully"
  return 0
}

# 构建msot包
function build_msot() {
  detect_msot_zone
  local msot_path="${CURRENT_DIR}/msot"

  if [[ -d "${CANN_3RD_LIB_PATH}/../mindstudio" ]]; then
    # 使用构建环境中的mindstudio源码
    log "Building msot with existing msot..."
    log "MindStudio path: ${MSOT_MINDSTUDIO_PATH}"
    log "CANN_3RD_LIB_PATH: ${CANN_3RD_LIB_PATH}"

    # msot源码路径
    local msot_src="${MSOT_MINDSTUDIO_PATH}/msot"

    if [[ ! -d "${msot_src}" ]]; then
      log "[ERROR] msot directory not found: ${msot_src}"
      exit 1
    fi

    # 创建所有软链
    create_msot_submodule_symlinks "${msot_src}" "${MSOT_MINDSTUDIO_PATH}"
    create_msopscommon_symlinks "${msot_src}" "${MSOT_MINDSTUDIO_PATH}"
    create_thirdparty_symlinks "${msot_src}"

    # 切换到msot目录，使用local模式构建
    cd "${msot_src}"
    python3 build.py local

    # 拷贝产物到asc-tools目录下
    local msot_output_src="${msot_src}/output"
    local msot_output_dst="${CURRENT_DIR}/mindstudio/msot/output"
    if [[ -d "${msot_output_src}" ]]; then
      log "Copying msot output to ${msot_output_dst}..."
      mkdir -p "${msot_output_dst}"
      cp -rf "${msot_output_src}/"*.run "${msot_output_dst}/"
      log "msot output copied successfully"
    else
      log "[WARNING] msot output directory not found: ${msot_output_src}"
    fi
  else
    # 使用git submodule
    log "Building msot with submodule..."

    # 1. 自动准备msot submodule（不存在则自动添加）
    if ! prepare_msot_submodule "${msot_path}"; then
      log "[ERROR] Failed to prepare msot submodule"
      exit 1
    fi

    # 2. 如果是git submodule，初始化（不递归）
    if [[ -f "${msot_path}/.git" ]]; then
      log "Initializing msot submodule (non-recursive)..."
      git submodule update --init --no-fetch msot
    fi

    # 3. 让msot的build.py处理所有其他依赖（makeself、子仓、msopscommon等）
    cd "${msot_path}"
    log "Running: python3 build.py"
    python3 build.py
  fi
}

main() {
  check_param_with_help "$@"
  set_options "$@"

  # --msot 必须与 --pkg 配合使用
  if [ "${MSOT}" == "true" ] && [ "${PKG}" != "true" ]; then
    log "[ERROR] --msot must be used with --pkg. Example: bash build.sh --pkg --msot"
    exit 1
  fi

  # --pkg --msot 构建msot包
  if [ "${PKG}" == "true" ] && [ "${MSOT}" == "true" ]; then
    build_msot
    exit 0
  fi

  set_env

  copy_deps_file

  clean

  if [ -n "${TEST}" ];then
    CUSTOM_OPTION="${CUSTOM_OPTION} -DENABLE_TEST=ON -DTEST_MOD=all"
    BUILD_TYPE="Debug"
  fi

  if [ -n "${TEST_PART}" ]; then
    CUSTOM_OPTION="${CUSTOM_OPTION} -DENABLE_TEST=ON"
    BUILD_TYPE="Debug"
  fi

  if [ "${ASAN}" == "true" ];then
    CUSTOM_OPTION="${CUSTOM_OPTION} -DENABLE_ASAN=true"
  fi

  if [ "${COV}" == "true" ];then
    CUSTOM_OPTION="${CUSTOM_OPTION} -DENABLE_GCOV=true"
  fi

  if [ "${PKG}" == "true" ];then
    CUSTOM_OPTION="${CUSTOM_OPTION} -DPACKAGE_OPEN_PROJECT=ON"
  fi

  CUSTOM_OPTION="${CUSTOM_OPTION} -DASCEND_CANN_PACKAGE_PATH=${ASCEND_CANN_PACKAGE_PATH}"
  CUSTOM_OPTION+=" -DCANN_3RD_LIB_PATH=${CANN_3RD_LIB_PATH}"
  CUSTOM_OPTION+=" -DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
  CUSTOM_OPTION+=" -DPACKAGE_TYPE=${PACKAGE_TYPE}"
  CUSTOM_OPTION+=" -DENABLE_MOD_EXT=${ENABLE_MOD_EXT}"

  if [[ ! -d "${BUILD_DIR}" ]]; then
    mkdir -p "${BUILD_DIR}"
  fi
  if [[ ! -d "${OUTPUT_DIR}" ]]; then
    mkdir -p "${OUTPUT_DIR}"
  fi

  cd ${BUILD_DIR}

  if [ -n "${TEST}" ]; then
    build_test
  elif [ -n "$TEST_PART" ]; then
    build_test_part
  else
    build_package
  fi
}

set -o pipefail

main "$@"
