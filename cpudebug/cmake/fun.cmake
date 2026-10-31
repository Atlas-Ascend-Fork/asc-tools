# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
function(gen_cmd_common target script config out_files)
  add_custom_command(OUTPUT ${out_files}
                     COMMAND python3 ${script} ${config} ${out_files}
                     DEPENDS ${script} ${config})
  add_custom_target(${target}
                    DEPENDS ${out_files})
endfunction()

function(gen_stubs target config stub_h)
  gen_cmd_common(${target} "${CPULIB_SRC_DIR}/model/scripts/write_stub.py" ${config} ${stub_h})
endfunction()

function(gen_intris target config func fmtc)
  gen_cmd_common(${target} "${CPULIB_SRC_DIR}/model/scripts/reg_funs_gen.py" ${config} "${func};${fmtc}")
endfunction()

function(gen_cce_stub target config stub_cc)
  gen_cmd_common(${target} "${CPULIB_SRC_DIR}/model/scripts/cce_stub.py" ${config} ${stub_cc})
endfunction()

function(gen_npuchk_stub target config stub_cc)
  gen_cmd_common(${target} "${CPULIB_SRC_DIR}/model/scripts/write_npuchk.py" ${config} ${stub_cc})
endfunction()

function(gen_merge_cfg target configs outcfg)
  gen_cmd_common(${target} "${CPULIB_SRC_DIR}/model/scripts/npucfg_merge.py" "${configs}" ${outcfg})
endfunction()

function(product_dir str newstr)
  if("x${str}" STREQUAL "xascend910")
    set(${newstr} "Ascend910A" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xascend310")
    set(${newstr} "Ascend310" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xascend310p")
    set(${newstr} "Ascend310P1" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xascend920")
    set(${newstr} "Ascend920A" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xascend910b")
    set(${newstr} "Ascend910B1" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xascend310B1")
    set(${newstr} "Ascend310B1" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xascend950pr_9599")
    set(${newstr} "Ascend950PR_9599" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xkirinx90")
    set(${newstr} "KirinX90" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xkirin9030")
    set(${newstr} "Kirin9030" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xkirindev0001")
    set(${newstr} "KirinDev0001" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xkirindev0002")
    set(${newstr} "KirinDev0002" PARENT_SCOPE)
  elseif("x${str}" STREQUAL "xkirindev0003")
    set(${newstr} "KirinDev0003" PARENT_SCOPE)
  else()
    string(SUBSTRING ${str} 0 1 _headlower)
    string(SUBSTRING ${str} 1 -1 _leftstr)
    string(TOUPPER ${_headlower} _headupper)
    set(${newstr} "${_headupper}${_leftstr}" PARENT_SCOPE)
  endif()
endfunction()
