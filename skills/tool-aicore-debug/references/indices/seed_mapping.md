# seed_mapping.md — ID 完整映射表

异常 ID 到种子目录的权威映射。诊断流程按 ID 定位 `references/detectors/runtime/{category}/{ID}.md`；种子目录名用于回溯 cann-aicore 源码。

## 列定义

| 列名 | 说明 |
|------|------|
| ID | 异常编号 = `references/detectors/runtime/{category}/` 下文件名去 `.md`，路由主键 |
| 完整目录名 | cann-aicore 代码库中种子目录的实际名称 |
| 类别目录 | 种子所属的类别子目录（9 类之一） |
| kernel_diff_type | `kernel_diff`（error/fixed 版 kernel 函数体有差异）/ `host_only_diff`（差异仅在 host 侧）/ `no_diff`（完全无差异） |
| behavior_note | 特殊行为备注，无则 "-" |

## 映射表（81 ID）

| ID | 完整目录名 | 类别目录 | kernel_diff_type | behavior_note |
|----|-----------|---------|------------------|---------------|
| illegal_instr | illegal_instr | alignment_config | kernel_diff | 主路径 asm volatile(".int 0x00000000") 注入全零机器码（探索性，`.word` 被 bisheng 拒绝，改用 `.int... |
| mte_atomic_misalign | mte_atomic_misalign | alignment_config | kernel_diff | SetAtomicAdd + DataCopy 写 GM，目标地址 +2 字节破坏 32B 对齐 |
| mte_instr_illegal_cfg | mte_instr_illegal_cfg | alignment_config | kernel_diff | 主路径 DataCopyPad padding=255 探索 L31/L55；OOB 兜底 DataCopy count=65535 致 UB OOB |
| tiling_overflow | tiling_size_verified | alignment_config | kernel_diff | - |
| tiling_ub_misalign | tiling_unalign_verified | alignment_config | host_only_diff | kernel 函数体与修复版完全一致，差异仅在 host 侧 |
| trap_instr | trap_instr | alignment_config | kernel_diff | 主路径 asm volatile("trap") 下发 trap 指令（探索性，trap 无 C API）；OOB 兜底 buf+65536 ≫ UB 声... |
| ub_addr_misalign | ub_unalign_verified | alignment_config | host_only_diff | kernel 函数体与修复版完全一致，差异仅在 host 侧 |
| cube_fixp_buffer_overflow | cube_fixp_buffer_overflow | cube | kernel_diff | loop 160 次 asc_mmad 累积 c 指针步长 256 floats，i≥128 起跨 128KB L0C 边界回转 |
| cube_fixp_overflow | cube_fixp_overflow | cube | kernel_diff | mSize=m*128=4096 与种子 Mmad 输出 m=32 不匹配；实际读跨度及 L78 归因须由布局、分配和日志确认 |
| cube_instr_illegal | cube_instr_illegal | cube | kernel_diff | 多文件 tiling 流水线；第 3 tile 注入 c 偏移 +1 元素（+4B）破坏 L0C 64B/16 元素对齐 |
| cube_l0a_wraparound | cube_l0a_wraparound | cube | kernel_diff | dst_gap=8192 × repeat=8 = 65536 half = 128KB ≫ L0A 64KB → 写址回转 |
| cube_l0b_wraparound | cube_l0b_wraparound | cube | kernel_diff | 多文件 GEMM tiling 流水线；深层 tile 2 注入 {repeat=8, dstGap=8192} → 8×8192=128KB ≫ L0B... |
| cube_l0c_overflow | cube_l0c_overflow | cube | kernel_diff | 单次 asc_mmad 写 cL0c+32640，m×n=256 floats 写入 [32640,32896) 跨 128KB 边界 128 float... |
| cube_nan_inf | cube_nan_input_verified | cube | host_only_diff | kernel 函数体与修复版完全一致，差异仅在 host 侧（NaN 输入由 host 侧填充 0xFF 触发） |
| l0a_rdwr_conflict | l0a_rw_conflict_verified | cube | kernel_diff | TC-Low 平坦 __cube__ 路径；asc_mmad 启动 PIPE_M 长读 L0A 后无 asc_sync() 立即 asc_copy_l12... |
| l0b_rdwr_conflict | l0b_rw_conflict_verified | cube | kernel_diff | 双错误码: 14/39 |
| mmad_illegal | mmad_zero_param_verified | cube | kernel_diff | - |
| datacopy_burst_len_illegal | datacopy_blocklen_verified | datacopy | kernel_diff | - |
| datacopy_burst_num_illegal | datacopy_blockcount_verified | datacopy | kernel_diff | - |
| datacopypad_ub_overflow | datacopypad_oob_verified | datacopy | kernel_diff | - |
| vec_mask_count_illegal | vecmask_zero_verified | datacopy | kernel_diff | - |
| atomic_fixpipe_overflow | fixpipe_atomic_oob_verified | fixpipe | kernel_diff | - |
| fixp_b4_misalign | fixp_b4_misalign | fixpipe | kernel_diff | unitFlag=4 未触发 L77，srcStride=3200 OOB 兜底覆盖 L78（L0C 读越界）；变体 1/2/0xFF hang（exit... |
| fixp_ch_split_illegal | fixp_ch_split_illegal | fixpipe | kernel_diff | isChannelSplit=true + half 输出未触发 L77（dav-2201 硬件静默忽略），srcStride=3200 OOB 兜底覆盖... |
| fixp_fbuf_overflow | fixp_fbuf_overflow | fixpipe | kernel_diff | asc_copy_l12fb 写 1×255×32B=8160B 从偏移 6656B 起 → 6656+8160=14816B > 7168B fbuf ... |
| fixp_l0c_rw_conflict | fixp_l0c_rw_conflict | fixpipe | kernel_diff | 移除 Mmad→Fixpipe 间 TQue 同步尝试 L74 冲突；srcStride=OOB_SRC_STRIDE(3200) 放大致 L0C 读越界... |
| fixp_l1_write_overflow | fixp_l1_write_overflow | fixpipe | kernel_diff | 读侧三参数同向放大（srcStride=3200 + ndNum=255 + srcNdStride=32768）致 L0C 读越界 L78；L80 经三... |
| fixpipe_addr_misalign | fixpipe_addr_misalign | fixpipe | kernel_diff | srcStride=3200（m=32×100）远超 1024-float CO1 buffer，单块 Fixpipe 读 L0C 越界 → L78 读地址溢出 |
| fixpipe_biu_error | fixpipe_biu_error | fixpipe | kernel_diff | 复用 W5a 骨架（合法 srcStride/dstStride），第 4 级 Fixpipe 处构造 badGm=baseAddr+(1ULL<<48)... |
| fixpipe_cfg_illegal | fixpipe_illegal_cfg_verified | fixpipe | kernel_diff | - |
| fixpipe_gm_overflow | fixpipe_write_oob_verified | fixpipe | kernel_diff | - |
| fixpipe_l0c_conflict | fixpipe_l0c_conflict | fixpipe | kernel_diff | dstStride=0 + nSize=n=32>16（ndNum=1 即非 nz2nd 模式，dst_d=0）致 Fixpipe 指令参数非法 → L7... |
| fixpipe_l1_overflow | fixpipe_read_l1_oob_verified | fixpipe | kernel_diff | - |
| fixpipe_l1_read_overflow | fixpipe_l1_read_overflow | fixpipe | kernel_diff | ndNum=4, srcNdStride=32768：第 1 块单独不溢出，第 2/3/4 块 L0C 读地址 32768/65536/98304 flo... |
| fixpipe_srcstride_overflow | fixpipe_srcstride_oob_verified | fixpipe | kernel_diff | 深度链种子；完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe），Fixpipe 阶段 fp.srcStride = m*... |
| c0_idx_overflow | ld3d_cidx_overflow_verified | loaddata | kernel_diff | - |
| first_window_overflow | ld3d_window_overrun_verified | loaddata | kernel_diff | - |
| fmap_size_illegal | ld3d_fmap_illegal_verified | loaddata | kernel_diff | - |
| l1_3d_size_illegal | ld3d_l1size_illegal_verified | loaddata | kernel_diff | - |
| load3d_chn_size_illegal | load3d_chn_size_illegal | loaddata | kernel_diff | l1_3d_size=17（half 类型 C0=16，17 非 16 对齐；area=64 小，L46 不触发）使 channelSize 对齐非法；实... |
| load3d_illegal_stride | load3d_illegal_stride | loaddata | kernel_diff | LoadData3D strideW/strideH=0 使步长为零，触发 AICoreError.md L45 ILLEGAL_STRIDE |
| load3d_km_start_illegal | load3d_km_start_illegal | loaddata | kernel_diff | LoadData3D mStartPt=200 使 M 方向起始位置超出合法范围，触发 AICoreError.md L47 ILLEGAL_K_M_ST... |
| load3d_km_step_illegal | load3d_km_step_illegal | loaddata | kernel_diff | kStartPt=136（16 对齐且 < k=144 使 L47 K_start 检查不触发）导致 LoadData3D 内部 K 方向读取地址不对齐 ... |
| loaddata2d_read_overflow | loaddata2d_oob_verified | loaddata | kernel_diff | - |
| loaddata3d_read_overflow | ld3d_read_oob_verified | loaddata | kernel_diff | - |
| loaddata3d_write_overflow | ld3d_write_oob_verified | loaddata | kernel_diff | - |
| dvg_stack_overflow | dvg_stack_overflow | memory_overflow | kernel_diff | 主路径 32 层宏嵌套 if 压栈探索 L158；OOB 兜底 buf+65536 >> UB 1KB 致 ADDR_MISALIGN |
| gm_overflow | gm_oob_verified | memory_overflow | kernel_diff | - |
| l2_write_overflow | l2_write_oob_verified | memory_overflow | kernel_diff | - |
| load2d_write_overflow | load2d_write_overflow | memory_overflow | kernel_diff | LoadData2D 单队列流水线，dstGap=128×repeatTimes=16 致 dst 写跨度 2304 half=4608 bytes 远超... |
| mte1_write_overflow | mte1_write_overflow | memory_overflow | kernel_diff | asc_copy_l12l0b dst_gap=8192 × repeat=8 = 128KB ≫ L0B 64KB；与 mte_mov_read_ove... |
| mte_gdma_read_overflow | mte_gdma_read_overflow | memory_overflow | kernel_diff | 8-tile VEC 双缓冲流水线；tileIdx==3 时 DataCopy src GM +0x100000=1MB 远超 2KB alloc |
| mte_mov_read_overflow | mte_mov_read_overflow | memory_overflow | kernel_diff | asc_copy_l12l0a 大 start_index=60000 读超 __cbuf__ L1 声明范围（256 half） |
| mte_read_ub_overflow | mte_read_ub_oob_verified | memory_overflow | kernel_diff | - |
| mte_write_ub_overflow | mte_write_ub_oob_verified | memory_overflow | kernel_diff | - |
| nd2nz_overflow | nd2nz_oob_verified | memory_overflow | kernel_diff | - |
| scalar_addr_error | scalar_addr_error | memory_overflow | kernel_diff | badPtr=xLocal+65536 偏移 256KB 远超 UB 声明 1KB，asc_copy_gm2ub_sync 向越界 UB 地址写入触发 U... |
| scalar_gm_addr_oob | scalar_gm_addr_oob | memory_overflow | kernel_diff | badPtr GM 地址 bit[48]≠0 超 48bit 物理地址空间，asc_copy_gm2ub_sync 经 badPtr 读 GM → SU ... |
| scalar_ub_forbidden | scalar_ub_forbidden | memory_overflow | kernel_diff | SCALAR volatile store 到 UB 下溢区域（buf-4096），CCU 指令级地址检查触发 data_exception；errcod... |
| ub_capacity_overflow | ub_overcap_verified | memory_overflow | kernel_diff | - |
| vec_gm_addr_oob | vec_gm_addr_oob | memory_overflow | kernel_diff | Init 阶段 SetGlobalBuffer 接受偏移指针致 GlobalTensor 基址 bit[48]≠0，Process 阶段 DataCopy... |
| vec_ub_forbidden_access | vec_ub_forbidden_access | memory_overflow | kernel_diff | VEC 计算路径 asc_add_sync src1 指向 UB 越界区域 buf+49152（192KB），CCU 指令级地址检查检测到 UB 地址越界... |
| vec_ub_overflow | vector_ub_oob_verified | memory_overflow | kernel_diff | - |
| ccu_div0 | scalar_divzero_verified | scalar_vec_compute | kernel_diff | - |
| scalar_fp_div0 | scalar_fp_div0 | scalar_vec_compute | kernel_diff | 语义意图 1.0f/0.0f 除零（静默产生 Inf，volatile 防常量折叠）；可靠后盾 `*oobPtr=divResult` SU store ... |
| scalar_fp_nan_inf | scalar_fp_nan_inf | scalar_vec_compute | kernel_diff | 语义意图对 NaN 输入执行 `nanVal + 1.0f`（静默传播 NaN）；可靠后盾 `*oobPtr=addResult` SU store 到 ... |
| scalar_fp_neg_sqrt | scalar_fp_neg_sqrt | scalar_vec_compute | kernel_diff | 当前种子为 `negVal * negVal`，-1.0f 的平方为 1.0f，未覆盖负数 SQRT；SU store OOB 的错误码待运行验证 |
| scalar_int_neg_sqrt | scalar_int_neg_sqrt | scalar_vec_compute | kernel_diff | 语义意图对 -16 执行 int sqrt 占位计算（TC-Low 无 int sqrt 标量 API）；可靠后盾 `*oobPtr=sqrtResult... |
| vec_div0 | vec_div0 | scalar_vec_compute | kernel_diff | count=65535 远超 UB tensor 声明 256 元素，repeat0 对除数 0.0f 执行 Div（语义意图，静默 Inf），repeat… |
| vec_ln_nonpositive | vec_ln_nonpositive | scalar_vec_compute | kernel_diff | count=65535 远超 UB tensor 声明 256 元素，repeat0 对非正数输入 0.0f 执行 Ln（语义意图，静默 NaN），rep... |
| vec_nan_inf | vec_nan_inf | scalar_vec_compute | kernel_diff | count=65535 远超 UB tensor 声明 256 元素，repeat0 对 NaN 输入执行 Muls（语义意图，静默传播 NaN），rep... |
| vec_neg_sqrt | vec_neg_sqrt | scalar_vec_compute | kernel_diff | count=65535 远超 UB tensor 声明 256 元素，repeat0 对负数输入 -1.0f 执行 Sqrt（语义意图，静默 NaN），r... |
| alloc_free_mismatch | alloc_free_mismatch_hangs | sync_hang | kernel_diff | kernel挂起型，无EZ9999输出 |
| enque_deque_mismatch | enque_deque_mismatch_hangs | sync_hang | kernel_diff | kernel挂起型，无EZ9999输出 |
| flag_counter_overflow | flag_counter_overflow | sync_hang | kernel_diff | 多文件结构（main.asc+kernel_class.h+fault_injector.h），tile 2 时注入器 16 次 SetFlag<MTE2... |
| flag_mismatch | flag_mismatch_hangs | sync_hang | kernel_diff | kernel挂起型，无EZ9999输出 |
| hset_counter_overflow | hset_counter_overflow | sync_hang | kernel_diff | 主路径 32 次 ffts_cross_core_sync arrive 不配对 wait 致 HSET counter 累积上溢（L8 探索）；OOB ... |
| padding_misconfig | ld3d_padding_cfg_hangs | sync_hang | kernel_diff | kernel hangs despite having error code 41 |
| atomic_dirty_cacheline | atomic_dirty_cacheline | system_bus | kernel_diff | 主路径 GM 标量 store 创建 dirty cacheline + atomic add 同址探索 L102；OOB 兜底 buf+65536 >>... |
| dcache_biu_response | dcache_biu_response | system_bus | kernel_diff | GM>48bit 构造；现存 asc_copy 片段与历史 scalar 描述冲突，L113 通道及独立覆盖待源码/日志确认 |
| mte_biu_rdwr_resp | mte_biu_rdwr_resp | system_bus | kernel_diff | GM>48bit 构造 + 4 次连续 MTE 读加重 BIU 读响应路径压力 |
