# keyword_index.md — 关键词索引

关键词到异常 ID 的倒排索引。diagnose 在没有可用的标量错误码匹配时，用显式 keyword（优先）或 error_str 抽取的关键词查表。未知错误码须说明已回退；挂起组合的优先级仍以 SKILL.md Step 1 为准。

> **设计说明**：左列关键词列按设计接受任意字符串（别名 / 种子目录名片段 / 报错原文片段 / API 名），不要求与 detector ID 一致。右列 `[...]` 内才是路由目标正式 ID。清理左列关键词会破坏别名查询能力（见 REDESIGN_AUDIT §三 P3 核查结论）。

| 关键词 | 异常 ID 列表 |
|--------|-------------|
| misalign | [ub_addr_misalign, tiling_ub_misalign] |
| align | [ub_addr_misalign, tiling_ub_misalign] |
| EZ9999 | [ub_addr_misalign, tiling_ub_misalign, gm_overflow, ub_capacity_overflow, tiling_overflow, l0a_rdwr_conflict, fixpipe_srcstride_overflow] |
| overflow | [gm_overflow, ub_capacity_overflow, loaddata2d_read_overflow, loaddata3d_write_overflow, loaddata3d_read_overflow, c0_idx_overflow, datacopypad_ub_overflow, nd2nz_overflow, vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, l2_write_overflow, fixpipe_gm_overflow, atomic_fixpipe_overflow, fixpipe_l1_overflow, fixpipe_srcstride_overflow, first_window_overflow, tiling_overflow, load2d_write_overflow, mte1_write_overflow, mte_gdma_read_overflow, mte_mov_read_overflow, cube_l0c_overflow, dvg_stack_overflow] |
| overflow,GM | [gm_overflow] |
| overflow,UB | [ub_capacity_overflow, vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, datacopypad_ub_overflow] |
| overflow,L2 | [l2_write_overflow] |
| overflow,L1 | [fixpipe_l1_overflow, fixpipe_srcstride_overflow] |
| overflow,FixPipe | [fixpipe_gm_overflow, atomic_fixpipe_overflow, fixpipe_l1_overflow, fixpipe_srcstride_overflow] |
| hangs | [flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig] |
| kernel_hangs | [flag_mismatch, alloc_free_mismatch, enque_deque_mismatch] |
| sync | [flag_mismatch, alloc_free_mismatch, enque_deque_mismatch] |
| flag | [flag_mismatch] |
| AllocTensor | [alloc_free_mismatch] |
| FreeTensor | [alloc_free_mismatch] |
| EnQue | [enque_deque_mismatch] |
| DeQue | [enque_deque_mismatch] |
| Queue | [alloc_free_mismatch, enque_deque_mismatch] |
| tiling | [tiling_ub_misalign, tiling_overflow] |
| DataCopy | [datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow] |
| blockLen | [datacopy_burst_len_illegal] |
| blockCount | [datacopy_burst_num_illegal] |
| DataCopyPad | [datacopypad_ub_overflow] |
| vec_mask | [vec_mask_count_illegal] |
| mask | [vec_mask_count_illegal] |
| cube_l0b_rdwr_cflt | [l0b_rdwr_conflict] |
| mte_l0b_rdwr_cflt | [l0b_rdwr_conflict] |
| L0B | [l0b_rdwr_conflict, cube_l0b_wraparound] |
| cube_l0a_rdwr_cflt | [l0a_rdwr_conflict] |
| L0A | [l0a_rdwr_conflict, cube_l0a_wraparound] |
| cube_illegal_instr | [mmad_illegal] |
| MMAD | [mmad_illegal] |
| Mmad | [mmad_illegal] |
| cube_invld_input | [cube_nan_inf] |
| NaN | [cube_nan_inf, vec_nan_inf] |
| LoadData2D | [loaddata2d_read_overflow] |
| mte_read_overflow | [loaddata2d_read_overflow] |
| LoadData | [loaddata3d_write_overflow, loaddata3d_read_overflow] |
| mte_write_3d_overflow | [loaddata3d_write_overflow] |
| mte_err_read_3d_overflow | [loaddata3d_read_overflow] |
| LoadData3D | [c0_idx_overflow, fmap_size_illegal, padding_misconfig, first_window_overflow, l1_3d_size_illegal, load3d_chn_size_illegal, load3d_illegal_stride, load3d_km_start_illegal, load3d_km_step_illegal] |
| mte_cidx_overflow | [c0_idx_overflow] |
| cidx | [c0_idx_overflow] |
| mte_illegal_fm_size | [fmap_size_illegal] |
| fmap | [fmap_size_illegal] |
| mte_padding_cfg | [padding_misconfig] |
| padding | [padding_misconfig] |
| mte_f1wpos_larger_fsize | [first_window_overflow] |
| mte_illegal_l1_3d_size | [l1_3d_size_illegal] |
| FixPipe | [fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow, fixpipe_srcstride_overflow, fixpipe_addr_misalign, fixpipe_l0c_conflict, fixpipe_biu_error, fixpipe_l1_read_overflow] |
| fixp_err_out_write_overflow | [fixpipe_gm_overflow, atomic_fixpipe_overflow] |
| fixp_err_illegal_cfg | [fixpipe_cfg_illegal] |
| fixp_err_read_l1_ovflw | [fixpipe_l1_overflow, fixpipe_srcstride_overflow] |
| AtomicAdd | [atomic_fixpipe_overflow] |
| srcStride | [fixpipe_srcstride_overflow] |
| Nd2Nz | [nd2nz_overflow] |
| mte_write_overflow | [nd2nz_overflow] |
| ccu_div0 | [ccu_div0] |
| div0 | [ccu_div0, vec_div0, scalar_fp_div0] |
| scalar | [ccu_div0, scalar_fp_div0, scalar_fp_nan_inf, scalar_fp_neg_sqrt, scalar_int_neg_sqrt, scalar_addr_error, scalar_gm_addr_oob, scalar_ub_forbidden] |
| biu_l2_write_oob | [l2_write_overflow] |
| vec_ub_addr_wrap_around | [vec_ub_overflow] |
| mte_ub_wr_ovflw | [datacopypad_ub_overflow, mte_write_ub_overflow] |
| mte_ub_rd_ovflw | [mte_read_ub_overflow] |
| atomic_dirty_cacheline | [atomic_dirty_cacheline] |
| Atomic | [atomic_dirty_cacheline, atomic_fixpipe_overflow] |
| dirty_cacheline | [atomic_dirty_cacheline] |
| cache | [atomic_dirty_cacheline, dcache_biu_response] |
| cube_fixp_buffer_overflow | [cube_fixp_buffer_overflow] |
| cube_fixp_overflow | [cube_fixp_overflow] |
| cube_instr_illegal | [cube_instr_illegal] |
| cube_l0a_wraparound | [cube_l0a_wraparound] |
| wraparound | [cube_l0a_wraparound, cube_l0b_wraparound] |
| cube_l0b_wraparound | [cube_l0b_wraparound] |
| cube_l0c_overflow | [cube_l0c_overflow] |
| L0C | [cube_l0c_overflow] |
| dcache_biu_response | [dcache_biu_response] |
| BIU | [dcache_biu_response, mte_biu_rdwr_resp, fixpipe_biu_error] |
| dvg_stack_overflow | [dvg_stack_overflow] |
| stack | [dvg_stack_overflow] |
| DVG | [dvg_stack_overflow] |
| fixp_b4_misalign | [fixp_b4_misalign] |
| b4_misalign | [fixp_b4_misalign] |
| fixp_ch_split_illegal | [fixp_ch_split_illegal] |
| ch_split | [fixp_ch_split_illegal] |
| fixp_fbuf_overflow | [fixp_fbuf_overflow] |
| fbuf | [fixp_fbuf_overflow] |
| fixp_l0c_rw_conflict | [fixp_l0c_rw_conflict] |
| fixp_l1_write_overflow | [fixp_l1_write_overflow] |
| fixpipe_addr_misalign | [fixpipe_addr_misalign] |
| fixpipe_biu_error | [fixpipe_biu_error] |
| fixpipe_l0c_conflict | [fixpipe_l0c_conflict] |
| fixpipe_l1_read_overflow | [fixpipe_l1_read_overflow] |
| flag_counter_overflow | [flag_counter_overflow] |
| counter | [flag_counter_overflow, hset_counter_overflow] |
| hset_counter_overflow | [hset_counter_overflow] |
| HSET | [hset_counter_overflow] |
| HWAIT | [hset_counter_overflow] |
| illegal_instr | [illegal_instr] |
| trap_instr | [trap_instr] |
| trap | [trap_instr] |
| instruction | [illegal_instr, trap_instr, cube_instr_illegal] |
| load2d_write_overflow | [load2d_write_overflow] |
| load3d_chn_size_illegal | [load3d_chn_size_illegal] |
| chn_size | [load3d_chn_size_illegal] |
| load3d_illegal_stride | [load3d_illegal_stride] |
| illegal_stride | [load3d_illegal_stride] |
| stride | [load3d_illegal_stride, fixpipe_srcstride_overflow] |
| load3d_km_start_illegal | [load3d_km_start_illegal] |
| mStartPt | [load3d_km_start_illegal] |
| load3d_km_step_illegal | [load3d_km_step_illegal] |
| kStartPt | [load3d_km_step_illegal] |
| mte1_write_overflow | [mte1_write_overflow] |
| mte_gdma_read_overflow | [mte_gdma_read_overflow] |
| gdma | [mte_gdma_read_overflow] |
| mte_mov_read_overflow | [mte_mov_read_overflow] |
| mte_atomic_misalign | [mte_atomic_misalign] |
| atomic_misalign | [mte_atomic_misalign] |
| mte_biu_rdwr_resp | [mte_biu_rdwr_resp] |
| mte_instr_illegal_cfg | [mte_instr_illegal_cfg] |
| INSTR_ILLEGAL_CFG | [mte_instr_illegal_cfg] |
| scalar_addr_error | [scalar_addr_error] |
| scalar_fp_div0 | [scalar_fp_div0] |
| Inf | [vec_nan_inf, scalar_fp_nan_inf, cube_nan_inf] |
| scalar_fp_nan_inf | [scalar_fp_nan_inf] |
| nan_inf | [vec_nan_inf, scalar_fp_nan_inf] |
| scalar_fp_neg_sqrt | [scalar_fp_neg_sqrt] |
| neg_sqrt | [vec_neg_sqrt, scalar_fp_neg_sqrt, scalar_int_neg_sqrt] |
| scalar_gm_addr_oob | [scalar_gm_addr_oob] |
| gm_addr_oob | [scalar_gm_addr_oob, vec_gm_addr_oob] |
| scalar_int_neg_sqrt | [scalar_int_neg_sqrt] |
| scalar_ub_forbidden | [scalar_ub_forbidden] |
| ub_forbidden | [scalar_ub_forbidden, vec_ub_forbidden_access] |
| forbidden | [scalar_ub_forbidden, vec_ub_forbidden_access] |
| vec_div0 | [vec_div0] |
| vec_gm_addr_oob | [vec_gm_addr_oob] |
| vec_ln_nonpositive | [vec_ln_nonpositive] |
| ln_nonpositive | [vec_ln_nonpositive] |
| vec_nan_inf | [vec_nan_inf] |
| vec_neg_sqrt | [vec_neg_sqrt] |
| vec_ub_forbidden_access | [vec_ub_forbidden_access] |
| CCU | [scalar_ub_forbidden, vec_ub_forbidden_access] |
