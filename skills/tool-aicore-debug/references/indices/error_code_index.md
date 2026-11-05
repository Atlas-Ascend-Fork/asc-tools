# error_code_index.md — 错误码索引

> **关键区分**：`error_code` = EZ9999 stderr 中的 errcode 字段值；`exit_code` = 操作系统进程退出码。两者独立，不存在转换关系。
>
> **路径约定**：所有异常 ID 对应 `references/detectors/runtime/{category}/{ID}.md`，category 见 `seed_mapping.md`。

输入只有报错原文时，先按 SKILL.md Step 1 提取独立十进制标量 errcode。未知标量码回退关键词；retCode、十六进制位图和三元组不能转换成此表的标量索引码。

## EZ9999 错误码到异常 ID 映射（error_code 路由）

| 错误码 | 异常 ID 列表 |
|--------|-------------|
| 1 | [l2_write_overflow] |
| 3 | [ccu_div0] |
| 9 | [cube_nan_inf] |
| 14 | [l0b_rdwr_conflict] |
| 24 | [c0_idx_overflow] |
| 26 | [first_window_overflow] |
| 30 | [datacopy_burst_len_illegal] |
| 31 | [datacopy_burst_num_illegal] |
| 35 | [fmap_size_illegal] |
| 36 | [l1_3d_size_illegal] |
| 39 | [l0b_rdwr_conflict] |
| 41 | [padding_misconfig] |
| 42 | [loaddata2d_read_overflow] |
| 47 | [loaddata3d_write_overflow] |
| 48 | [nd2nz_overflow] |
| 53 | [vec_mask_count_illegal] |
| 93 | [loaddata3d_read_overflow] |
| 101 | [fixpipe_cfg_illegal] |
| 103 | [fixpipe_l1_overflow] |
| 124 | [datacopypad_ub_overflow, mte_write_ub_overflow] |
| 125 | [mte_read_ub_overflow] |
| 126 | [mmad_illegal] |
| 141 | [fixpipe_gm_overflow, atomic_fixpipe_overflow] |
| 154 | [vec_ub_overflow] |

## 进程退出码索引（exit_code 路由）

当 EZ9999 错误码不可用（stderr 为空）时，通过进程退出码路由：

| exit_code | 含义 | 异常 ID 列表 | 特征 |
|-----------|------|-------------|------|
| **124** | **超时挂起（timeout）** | **[flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig]** | **stderr 完全为空，kernel 死锁/永等** |
| 1 | 异常退出，不能单独确认 AI Core 错误 | （需配合 error_code 或 error_str 路由） | 需核对 stderr 的 AI Core 证据 |
| 0 | 正常退出 | — | 无异常 |

> **注意**：EZ9999 error_code=124（UB 溢出，exit=1）与 exit_code=124（超时挂起，无 stderr）是完全不同的异常。前者从 stderr 的 errcode 字段获取，后者从 `$?` 获取。

## 碰撞 / 双码标注

| 错误码 | 异常 | 说明 |
|--------|------|------|
| 141 | fixpipe_gm_overflow, atomic_fixpipe_overflow | FixPipe 写 GM 溢出 与 Fixpipe AtomicAdd GM 溢出 共享错误码 141 |
| 124 | datacopypad_ub_overflow, mte_write_ub_overflow | DataCopyPad 参数导致 UB 写越界 与 MTE 写 UB 地址溢出 共享错误码 124 |
| 14 / 39 | l0b_rdwr_conflict | 同一异常关联两个错误码（cube_l0b_rdwr_cflt / mte_l0b_rdwr_cflt） |
| 41 | padding_misconfig | README 声明码 41，但实际行为为 kernel 挂起（双路由：error_code=41 与 exit_code=124 均可命中，优先按 error_code=41 路由并标记 `kernel_may_hang=true`） |

## 无数值型错误码异常（56 ID）

以下异常在 stderr 中无数值型 EZ9999 errcode（或为 hex/描述性文本），路由依赖 `error_str` 抽关键词查 `keyword_index.md`，或 `exit_code=124`（挂起型）。

### kernel 挂起型（exit_code=124 路由）

| 异常 ID | 报错模式 |
|---------|---------|
| flag_mismatch | kernel_hangs（exit_code=124 路由） |
| alloc_free_mismatch | kernel_hangs（exit_code=124 路由） |
| enque_deque_mismatch | kernel_hangs（exit_code=124 路由） |

### EZ9999 无数值型码（描述性文本，按 keyword 路由）

| 异常 ID | 报错模式 |
|---------|---------|
| ub_addr_misalign | EZ9999（无数值型错误码） |
| tiling_ub_misalign | EZ9999（无数值型错误码） |
| gm_overflow | EZ9999（无数值型错误码） |
| ub_capacity_overflow | EZ9999（无数值型错误码） |
| tiling_overflow | EZ9999（无数值型错误码） |
| l0a_rdwr_conflict | EZ9999（冒号分隔描述性文本） |
| fixpipe_srcstride_overflow | EZ9999（errcode 描述性文本 `(0, 0x4000000000, 0)`，subErrType=4） |

### EZ9999 含 retCode / aic error mask / hex errcode（无数值型 EZ9999 码，按 keyword 路由）

| 异常 ID | 报错模式 |
|---|---|
| atomic_dirty_cacheline | EZ9999（验证阶段补充 retCode/errcode/subErrType/vec error info 字段） |
| cube_fixp_buffer_overflow | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c |
| cube_fixp_overflow | EZ9999 + retCode=0x26 [aicore exception] + subErrType=4 |
| cube_instr_illegal | EZ9999 + retCode=0x26 [aicore exception] + cube error info 非零 |
| cube_l0a_wraparound | EZ9999 + retCode=0x26 [aicore exception] + cube error info 非零 |
| cube_l0b_wraparound | EZ9999 + retCode=0x26 [aicore exception] + cube error info 非零 |
| cube_l0c_overflow | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c |
| dcache_biu_response | EZ9999（验证阶段补充 retCode/errcode 字段） |
| dvg_stack_overflow | EZ9999（验证阶段补充 retCode/errcode/subErrType/vec error info 字段） |
| fixp_b4_misalign | EZ9999 + retCode=0x26 + aic error mask=0x6500020bd00028c + fixp_error0=0x9e + fixp_error1=0x2 + subErrType=4 |
| fixp_ch_split_illegal | EZ9999 + retCode=0x26 + aic error mask=0x6500020bd00028c + fixp_error0=0xa0 + fixp_error1=0x2 + subErrType=4 |
| fixp_fbuf_overflow | EZ9999 + retCode=0x26 [aicore exception] |
| fixp_l0c_rw_conflict | EZ9999 + retCode=0x26 [aicore exception] |
| fixp_l1_write_overflow | EZ9999 + retCode=0x26 [aicore exception] |
| fixpipe_addr_misalign | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c + subErrType=4 |
| fixpipe_biu_error | EZ9999 + retCode=0x26 [aicore exception] |
| fixpipe_l0c_conflict | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c + subErrType=4 |
| fixpipe_l1_read_overflow | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c + subErrType=4 |
| flag_counter_overflow | EZ9999（验证阶段补充 retCode/errcode 字段；#2 预期高概率剔除） |
| hset_counter_overflow | EZ9999（验证阶段补充 retCode/errcode/subErrType/cube error info 字段） |
| illegal_instr | EZ9999（验证阶段回填 retCode/exit_code/errcode/subErrType/ifu error info） |
| load2d_write_overflow | EZ9999 + errcode (0x1000000000000, 0, 0) |
| load3d_chn_size_illegal | EZ9999 + errcode (0, 0x10000, 0) ADDR_MISALIGN |
| load3d_illegal_stride | EZ9999（验证阶段补充 retCode/errcode 字段） |
| load3d_km_start_illegal | EZ9999（验证阶段补充 retCode/exit_code 字段） |
| load3d_km_step_illegal | EZ9999=YES + retCode=0x26 + errorStr="instruction address misalign(ADDR_MISALIGN)" + mte error info=0x1000000e5 + runtime result=507015 |
| mte1_write_overflow | EZ9999 + retCode=0x26 [aicore exception] + errcode=(0x1000000000000,0,0) |
| mte_atomic_misalign | EZ9999 + retCode=0x31 [vector core exception] + subErrType=4 |
| mte_biu_rdwr_resp | EZ9999 + retCode=0x31 [vector core exception] |
| mte_gdma_read_overflow | EZ9999 + retCode=0x31 [vector core exception] |
| mte_instr_illegal_cfg | EZ9999 + retCode=0x31 [vector core exception] + aic error mask=0x6500020bd00028c |
| mte_mov_read_overflow | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c |
| scalar_addr_error | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| scalar_fp_div0 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| scalar_fp_nan_inf | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| scalar_fp_neg_sqrt | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| scalar_gm_addr_oob | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| scalar_int_neg_sqrt | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| scalar_ub_forbidden | EZ9999 + retCode=0x31 + errcode:(0, 0x4000, 0) "CCU instruction address check error." |
| trap_instr | EZ9999（验证阶段回填 retCode/exit_code/errcode/subErrType/vec error info） |
| vec_div0 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| vec_gm_addr_oob | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| vec_ln_nonpositive | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| vec_nan_inf | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| vec_neg_sqrt | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| vec_ub_forbidden_access | EZ9999 + retCode=0x31 + errcode:(0x4000000000000000, 0x4000, 0) "VEC instruction error: the ub address out of bounds. CCU instruction address check error." |
