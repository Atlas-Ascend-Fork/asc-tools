# npu-compute performance data

This document describes the fields and calculations in Section CSV files, `summary.jsonl`, and `HardwareInfo.jsonl`. CSV files contain Block/sub-block details. The summary contains Task-level metrics for the selected Sections and an `OpInfoSummary` operator overview. HardwareInfo records host and device information.

| Output | Scope | Relationship to other data |
|---|---|---|
| Section CSV | Block/sub-block | Counters, execution times, and derived metrics for each logical execution object. |
| Section records in the summary | Task | Cycles, counters, and data volumes correspond to cumulative totals. Ratios, hit rates, and bandwidths use cumulative inputs and their respective denominators; they are not uniformly Block averages. |
| `OpInfoSummary` | Current Kernel | Combines Task metrics, Block parallel statistics, runtime metadata, and hardware capabilities. |
| `HardwareInfo.jsonl` | Collection environment | Host and device information, distinct from the Kernel's logical launch counts. |

Matching metric names use the same units and base formulas, but their scope, frequency, or time denominator may differ. See the summary section for the relationship between CSV files and the summary.

## ArithmeticUtilization (Cube and Vector instruction utilization and counts)

ArithmeticUtilization.csv contains active-cycle ratios for Cube and Vector instructions and instruction counts for Cube. Use it to examine activity by instruction type and the number of Cube instructions executed. See the field descriptions in the table below for details.

NA means inapplicable to the current Core or unavailable data; it does not mean zero.

| Field | Description |
|---|---|
| `block_id` | Logical Block identifier for this row. |
| `sub_block_id` | Core type and Sub-block identifier: cubeN for AI Cube Core or vectorN for AI Vector Core. N is the Sub-block number, not the physical Core number. |
| `aic_time(us)` | Execution time on the AI Cube Core for this row, converted from total cycles using its frequency, in us. |
| `aic_total_cycles` | Total execution cycles on the AI Cube Core for this row. |
| `aic_cube_ratio` | Cube active cycles as a fraction of total AI Cube Core cycles. |
| `aic_cube_fp_ratio` | Cube floating-point instructions active cycles as a fraction of total AI Cube Core cycles. |
| `aic_cube_int_ratio` | Cube integer instructions active cycles as a fraction of total AI Cube Core cycles. |
| `aic_cube_total_instr_number` | Total number of instructions executed by Cube. |
| `aic_cube_fp_instr_number` | Number of floating-point instructions executed by Cube. |
| `aic_cube_int_instr_number` | Number of integer instructions executed by Cube. |
| `aiv_time(us)` | Execution time on the AI Vector Core for this row, converted from total cycles using its frequency, in us. |
| `aiv_total_cycles` | Total execution cycles on the AI Vector Core for this row. |
| `aiv_vec_ratio` | Vector active cycles as a fraction of total AI Vector Core cycles. |
| `aiv_vec_sfu_ratio` | Vector SFU instructions active cycles as a fraction of total AI Vector Core cycles. |
| `aiv_simd_vf_ratio` | AIV SIMD VF instruction issue cycles as a fraction of total AIV cycles. |
| `aiv_simt_vf_ratio` | AIV SIMT VF instruction issue cycles as a fraction of total AIV cycles. |
| `aiv_simd_vf_time` | Total time spent issuing AIV SIMD VF instructions, in us. |
| `aiv_simd_vf_ipc` | Average number of AIV SIMD VF instructions issued per cycle. |
| `aiv_simt_vf_time` | Total time spent issuing AIV SIMT VF instructions, in us. |
| `aiv_simt_vf_ipc` | Average number of AIV SIMT VF instructions issued per cycle. |

## L2Cache (L2 cache access counts and hit rates)

L2Cache.csv contains L2 read/write counts and hit rates. Use it to examine hits, misses, and evictions for near and far L2 Cache accesses by AI Cube Core and AI Vector Core. See the field descriptions in the table below for details.

NA means inapplicable to the current Core or unavailable data; it does not mean zero.

| Field | Description |
|---|---|
| `block_id` | Logical Block identifier for this row. |
| `sub_block_id` | Core type and Sub-block identifier: cubeN for AI Cube Core or vectorN for AI Vector Core. N is the Sub-block number, not the physical Core number. |
| `aic_time(us)` | Execution time on the AI Cube Core for this row, converted from total cycles using its frequency, in us. |
| `aic_total_cycles` | Total execution cycles on the AI Cube Core for this row. |
| `aic_read_close_hit` | Number of near L2 read hits for the AI Cube Core. |
| `aic_read_close_miss` | Number of near L2 read misses for the AI Cube Core. |
| `aic_read_close_victim` | Number of near L2 read evictions for the AI Cube Core. |
| `aic_read_far_hit` | Number of far L2 read hits for the AI Cube Core. |
| `aic_read_far_miss` | Number of far L2 read misses for the AI Cube Core. |
| `aic_read_far_victim` | Number of far L2 read evictions for the AI Cube Core. |
| `aic_read_hit_rate(%)` | L2 read hit rate for the AI Cube Core, in %. The hit count includes near and far hits; the total includes near and far hits, misses, and evictions. A complete set of counters with zero total produces 0. |
| `aic_write_close_hit` | Number of near L2 write hits for the AI Cube Core. |
| `aic_write_close_miss` | Number of near L2 write misses for the AI Cube Core. |
| `aic_write_close_victim` | Number of near L2 write evictions for the AI Cube Core. |
| `aic_write_far_hit` | Number of far L2 write hits for the AI Cube Core. |
| `aic_write_far_miss` | Number of far L2 write misses for the AI Cube Core. |
| `aic_write_far_victim` | Number of far L2 write evictions for the AI Cube Core. |
| `aic_write_hit_rate(%)` | L2 write hit rate for the AI Cube Core, in %. The hit count includes near and far hits; the total includes near and far hits, misses, and evictions. A complete set of counters with zero total produces 0. |
| `aiv_time(us)` | Execution time on the AI Vector Core for this row, converted from total cycles using its frequency, in us. |
| `aiv_total_cycles` | Total execution cycles on the AI Vector Core for this row. |
| `aiv_read_close_hit` | Number of near L2 read hits for the AI Vector Core. |
| `aiv_read_close_miss` | Number of near L2 read misses for the AI Vector Core. |
| `aiv_read_close_victim` | Number of near L2 read evictions for the AI Vector Core. |
| `aiv_read_far_hit` | Number of far L2 read hits for the AI Vector Core. |
| `aiv_read_far_miss` | Number of far L2 read misses for the AI Vector Core. |
| `aiv_read_far_victim` | Number of far L2 read evictions for the AI Vector Core. |
| `aiv_read_hit_rate(%)` | L2 read hit rate for the AI Vector Core, in %. The hit count includes near and far hits; the total includes near and far hits, misses, and evictions. A complete set of counters with zero total produces 0. |
| `aiv_write_close_hit` | Number of near L2 write hits for the AI Vector Core. |
| `aiv_write_close_miss` | Number of near L2 write misses for the AI Vector Core. |
| `aiv_write_close_victim` | Number of near L2 write evictions for the AI Vector Core. |
| `aiv_write_far_hit` | Number of far L2 write hits for the AI Vector Core. |
| `aiv_write_far_miss` | Number of far L2 write misses for the AI Vector Core. |
| `aiv_write_far_victim` | Number of far L2 write evictions for the AI Vector Core. |
| `aiv_write_hit_rate(%)` | L2 write hit rate for the AI Vector Core, in %. The hit count includes near and far hits; the total includes near and far hits, misses, and evictions. A complete set of counters with zero total produces 0. |

## Memory (Memory bandwidth and transfer volume)

Memory.csv contains main-memory, L1, and UB bandwidth, transfer volumes, and transfer instruction information. Use it to examine main-memory reads and writes and transfers along the data paths. See the field descriptions in the table below for details.

Bandwidth is calculated from transferred data and execution time, in GB/s. Mixed Kernels use a valid Task duration when available; otherwise the current Core execution time is used.

NA means inapplicable to the current Core or unavailable data; it does not mean zero.

| Field | Description |
|---|---|
| `block_id` | Logical Block identifier for this row. |
| `sub_block_id` | Core type and Sub-block identifier: cubeN for AI Cube Core or vectorN for AI Vector Core. N is the Sub-block number, not the physical Core number. |
| `aic_time(us)` | Execution time on the AI Cube Core for this row, converted from total cycles using its frequency, in us. |
| `aic_total_cycles` | Total execution cycles on the AI Cube Core for this row. |
| `aic_l1_read_bw(GB/s)` | Average bandwidth for reading data from L1, in GB/s. |
| `aic_l1_write_bw(GB/s)` | Average bandwidth for writing data into L1, in GB/s. |
| `aic_main_mem_read_bw(GB/s)` | Average main-memory read bandwidth of the AI Cube Core, in GB/s. |
| `aic_main_mem_write_bw(GB/s)` | Average main-memory write bandwidth of the AI Cube Core, in GB/s. |
| `aic_mte1_instructions` | Number of MTE1 instructions executed by the AI Cube Core. |
| `aic_mte1_ratio` | MTE1 active cycles as a fraction of total AI Cube Core cycles. |
| `aic_mte2_instructions` | Number of MTE2 instructions executed by the AI Cube Core. |
| `aic_mte2_ratio` | MTE2 active cycles as a fraction of total AI Cube Core cycles. |
| `aic_mte3_instructions` | Number of MTE3 instructions executed by the AI Cube Core. |
| `aic_mte3_ratio` | MTE3 active cycles as a fraction of total AI Cube Core cycles. |
| `aiv_time(us)` | Execution time on the AI Vector Core for this row, converted from total cycles using its frequency, in us. |
| `aiv_total_cycles` | Total execution cycles on the AI Vector Core for this row. |
| `aiv_ub_to_gm_bw(GB/s)` | Bandwidth for transferring data from UB to GM, in GB/s. Currently always NA because the required instrumentation data is unavailable. |
| `aiv_gm_to_ub_bw(GB/s)` | Average bandwidth for transferring data from GM to UB, in GB/s. |
| `aiv_main_mem_read_bw(GB/s)` | Average main-memory read bandwidth of the AI Vector Core, in GB/s. |
| `aiv_main_mem_write_bw(GB/s)` | Average main-memory write bandwidth of the AI Vector Core, in GB/s. |
| `aiv_mte2_instructions` | Number of MTE2 instructions executed by the AI Vector Core. |
| `aiv_mte2_ratio` | MTE2 active cycles as a fraction of total AI Vector Core cycles. |
| `aiv_mte3_instructions` | Number of MTE3 instructions executed by the AI Vector Core. |
| `aiv_mte3_ratio` | MTE3 active cycles as a fraction of total AI Vector Core cycles. |
| `read_main_memory_datas(KB)` | Amount of main-memory data read by the Core represented by this row, in KB. |
| `write_main_memory_datas(KB)` | Amount of main-memory data written by the Core represented by this row, in KB. |
| `GM_to_L1_datas(KB)` | Amount of data transferred from GM to L1, in KB, for the current Core row. |
| `GM_to_L1_bw_usage_rate(%)` | Average bandwidth from GM to L1 as a percentage of the configured maximum for that path, capped at 100%. |
| `L0C_to_L1_datas(KB)` | Amount of data transferred from L0C to L1, in KB, for the current Core row. |
| `L0C_to_L1_bw_usage_rate(%)` | Average bandwidth from L0C to L1 as a percentage of the configured maximum for that path, capped at 100%. |
| `L0C_to_GM_datas(KB)` | Amount of data transferred from L0C to GM, in KB, for the current Core row. |
| `L0C_to_GM_bw_usage_rate(%)` | Average bandwidth from L0C to GM as a percentage of the configured maximum for that path, capped at 100%. |
| `GM_to_UB_datas(KB)` | Amount of data transferred from GM to UB, in KB, for the current Core row. |
| `GM_to_UB_bw_usage_rate(%)` | Average bandwidth from GM to UB as a percentage of the configured maximum for that path, capped at 100%. |
| `UB_to_GM_datas(KB)` | Amount of data transferred from UB to GM, in KB. Currently always NA because the required instrumentation data is unavailable. |
| `UB_to_GM_bw_usage_rate(%)` | Bandwidth utilization of the UB-to-GM path, expressed as a percentage. Currently always NA because the required instrumentation data is unavailable. |

GM-to-UB volume uses the main-memory read count after subtracting two DCache-miss-related read counts, with negative differences treated as zero. GM-to-L1 and L0C-to-GM use the AIC main-memory read and write counts, respectively.

## MemoryL0 (L0 read and write bandwidth)

MemoryL0.csv contains bandwidth between L0A, L0B, L0C and Cube or L1. Use it to examine the average bandwidth of each data path. See the field descriptions in the table below for details.

Bandwidth fields apply to AI Cube Core. Bandwidth is calculated from transferred data and execution time, in GB/s. Mixed Kernels use a valid Task duration when available; otherwise the current Core execution time is used.

NA means inapplicable to the current Core or unavailable data; it does not mean zero.

| Field | Description |
|---|---|
| `block_id` | Logical Block identifier for this row. |
| `sub_block_id` | Core type and Sub-block identifier: cubeN for AI Cube Core or vectorN for AI Vector Core. N is the Sub-block number, not the physical Core number. |
| `aic_time(us)` | Execution time on the AI Cube Core for this row, converted from total cycles using its frequency, in us. |
| `aic_total_cycles` | Total execution cycles on the AI Cube Core for this row. |
| `aic_l0a_read_bw(GB/s)` | Average bandwidth for transferring data from L0A to Cube, in GB/s. |
| `aic_l0a_write_bw(GB/s)` | Average bandwidth for transferring data from L1 to L0A, in GB/s. |
| `aic_l0b_read_bw(GB/s)` | Average bandwidth for transferring data from L0B to Cube, in GB/s. |
| `aic_l0b_write_bw(GB/s)` | Average bandwidth for transferring data from L1 to L0B, in GB/s. |
| `aic_l0c_read_bw_cube(GB/s)` | Average bandwidth for Cube reading data from L0C, in GB/s. |
| `aic_l0c_write_bw_cube(GB/s)` | Average bandwidth for Cube writing data into L0C, in GB/s. |
| `aiv_time(us)` | Execution time on the AI Vector Core for this row, converted from total cycles using its frequency, in us. |
| `aiv_total_cycles` | Total execution cycles on the AI Vector Core for this row. |

## MemoryUB (UB read and write bandwidth)

MemoryUB.csv contains bandwidth between UB and Vector or GM. Use it to examine the average bandwidth of Vector accesses to UB and transfers from GM to UB. See the field descriptions in the table below for details.

Bandwidth fields apply to AI Vector Core. Bandwidth is calculated from transferred data and execution time, in GB/s. Mixed Kernels use a valid Task duration when available; otherwise the current Core execution time is used.

NA means inapplicable to the current Core or unavailable data; it does not mean zero.

| Field | Description |
|---|---|
| `block_id` | Logical Block identifier for this row. |
| `sub_block_id` | Core type and Sub-block identifier: cubeN for AI Cube Core or vectorN for AI Vector Core. N is the Sub-block number, not the physical Core number. |
| `aic_time(us)` | Execution time on the AI Cube Core for this row, converted from total cycles using its frequency, in us. |
| `aic_total_cycles` | Total execution cycles on the AI Cube Core for this row. |
| `aiv_time(us)` | Execution time on the AI Vector Core for this row, converted from total cycles using its frequency, in us. |
| `aiv_total_cycles` | Total execution cycles on the AI Vector Core for this row. |
| `aiv_ub_read_bw_vector(GB/s)` | Average bandwidth for Vector reading data from UB, in GB/s. |
| `aiv_ub_write_bw_vector(GB/s)` | Average bandwidth for Vector writing data into UB, in GB/s. |
| `aiv_ub_read_bw_gm(GB/s)` | Bandwidth for reading UB data for transfer to GM, in GB/s. Currently always NA because the required instrumentation data is unavailable. |
| `aiv_ub_write_bw_gm(GB/s)` | Average bandwidth for transferring data from GM into UB, in GB/s. |

## PcSampling (PC sampling state totals)

Instruction addresses and state totals collected by `PmSampling` are stored in the `PcSampling` table in `summary.db`. `address` stores the business instruction address, and records for the same address are summed separately for each state.

| Field | Description |
|---|---|
| `address` | Business instruction address. |
| `ibuf_empty` | The instruction buffer is empty, waiting for the next instruction. |
| `nop_cycles` | Waiting caused by no-operation (NOP) instructions. |
| `scoreboard_not_ready` | The scoreboard is not ready; the instruction waits for data or resources from previous instructions. |
| `register_bank_conflict` | Register accesses contend for the same register bank, causing a stall. |
| `resource_conflict` | Multiple instructions contend for the same hardware resource, causing a stall. |
| `warp_level_sync` | Waiting for threads in the same warp to meet synchronization conditions. |
| `divergence_stack_spill` | Waiting for spill handling when the divergence stack has insufficient space. |
| `others` | Other waiting or stall states outside the categories above. |

## PipeUtilization (Compute and transfer pipeline utilization)

PipeUtilization.csv contains compute and transfer pipeline active times, cycle ratios, active bandwidth, and instruction-cache miss rates. Use it to examine pipeline active times and cycle ratios. See the field descriptions in the table below for details.

Active bandwidth uses the corresponding pipeline active time. Some bandwidth fields also require data from Memory or MemoryUB and may be NA when this Section is collected alone.

NA means inapplicable to the current Core or unavailable data; it does not mean zero.

| Field | Description |
|---|---|
| `block_id` | Logical Block identifier for this row. |
| `sub_block_id` | Core type and Sub-block identifier: cubeN for AI Cube Core or vectorN for AI Vector Core. N is the Sub-block number, not the physical Core number. |
| `aic_time(us)` | Execution time on the AI Cube Core for this row, converted from total cycles using its frequency, in us. |
| `aic_total_cycles` | Total execution cycles on the AI Cube Core for this row. |
| `aic_cube_time(us)` | Active time of the Cube pipeline on the AI Cube Core, in us. |
| `aic_cube_ratio` | Cube active cycles as a fraction of total AI Cube Core cycles. |
| `aic_scalar_time(us)` | Active time of the Scalar pipeline on the AI Cube Core, in us. |
| `aic_scalar_ratio` | Scalar active cycles as a fraction of total AI Cube Core cycles. |
| `aic_mte1_time(us)` | Active time of the MTE1 pipeline on the AI Cube Core, in us. |
| `aic_mte1_ratio` | MTE1 active cycles as a fraction of total AI Cube Core cycles. |
| `aic_mte1_active_bw(GB/s)` | Bandwidth for reading L1 data during MTE1 activity, in GB/s. |
| `aic_mte2_time(us)` | Active time of the MTE2 pipeline on the AI Cube Core, in us. |
| `aic_mte2_ratio` | MTE2 active cycles as a fraction of total AI Cube Core cycles. |
| `aic_mte2_active_bw(GB/s)` | Bandwidth for reading main-memory data during MTE2 activity, in GB/s. |
| `aic_mte3_time(us)` | Active time of the MTE3 pipeline on the AI Cube Core, in us. |
| `aic_mte3_ratio` | MTE3 active cycles as a fraction of total AI Cube Core cycles. |
| `aic_mte3_active_bw(GB/s)` | Bandwidth for writing L1 data during MTE3 activity, using the adjusted L1 write count, in GB/s. |
| `aic_fixpipe_time(us)` | Active time of the Fixpipe pipeline on the AI Cube Core, in us. |
| `aic_fixpipe_ratio` | Fixpipe active cycles as a fraction of total AI Cube Core cycles. |
| `aic_fixpipe_active_bw(GB/s)` | Bandwidth for Fixpipe transfers during Fixpipe activity, in GB/s. |
| `aic_icache_miss_rate` | Number of instruction-cache misses divided by the total number of accesses for the AI Cube Core. |
| `aiv_time(us)` | Execution time on the AI Vector Core for this row, converted from total cycles using its frequency, in us. |
| `aiv_total_cycles` | Total execution cycles on the AI Vector Core for this row. |
| `aiv_vec_time(us)` | Active time of the Vector pipeline on the AI Vector Core, in us. |
| `aiv_vec_ratio` | Vector active cycles as a fraction of total AI Vector Core cycles. |
| `aiv_scalar_time(us)` | Active time of the Scalar pipeline on the AI Vector Core, in us. |
| `aiv_scalar_ratio` | Scalar active cycles as a fraction of total AI Vector Core cycles. |
| `aiv_mte2_time(us)` | Active time of the MTE2 pipeline on the AI Vector Core, in us. |
| `aiv_mte2_ratio` | MTE2 active cycles as a fraction of total AI Vector Core cycles. |
| `aiv_mte2_active_bw(GB/s)` | Bandwidth for GM-to-UB transfers during MTE2 activity, in GB/s. |
| `aiv_mte3_time(us)` | Active time of the MTE3 pipeline on the AI Vector Core, in us. |
| `aiv_mte3_ratio` | MTE3 active cycles as a fraction of total AI Vector Core cycles. |
| `aiv_mte3_active_bw(GB/s)` | Bandwidth during AI Vector Core MTE3 activity, in GB/s. Currently always NA because the required instrumentation data is unavailable. |
| `aiv_icache_miss_rate` | Number of instruction-cache misses divided by the total number of accesses for the AI Vector Core. |

## ResourceConflictRatio (Resource conflict and wait ratios)

ResourceConflictRatio.csv contains compute and transfer wait-cycle ratios and Vector resource-conflict ratios. Use it to examine waits in each unit and Vector resource conflicts. See the field descriptions in the table below for details.

NA means inapplicable to the current Core or unavailable data; it does not mean zero.

| Field | Description |
|---|---|
| `block_id` | Logical Block identifier for this row. |
| `sub_block_id` | Core type and Sub-block identifier: cubeN for AI Cube Core or vectorN for AI Vector Core. N is the Sub-block number, not the physical Core number. |
| `aic_time(us)` | Execution time on the AI Cube Core for this row, converted from total cycles using its frequency, in us. |
| `aic_total_cycles` | Total execution cycles on the AI Cube Core for this row. |
| `aic_cube_wait_ratio` | Cube wait cycles as a fraction of total AI Cube Core cycles. |
| `aic_mte1_wait_ratio` | MTE1 wait cycles as a fraction of total AI Cube Core cycles. |
| `aic_mte2_wait_ratio` | MTE2 wait cycles as a fraction of total AI Cube Core cycles. |
| `aic_mte3_wait_ratio` | MTE3 wait cycles as a fraction of total AI Cube Core cycles. |
| `aiv_time(us)` | Execution time on the AI Vector Core for this row, converted from total cycles using its frequency, in us. |
| `aiv_total_cycles` | Total execution cycles on the AI Vector Core for this row. |
| `aiv_vec_stu_cflt_ratio` | Vector STU conflict cycles as a fraction of total AI Vector Core cycles. |
| `aiv_vec_ldu_cflt_ratio` | Vector LDU conflict cycles as a fraction of total AI Vector Core cycles. |
| `aiv_vec_sfu_cflt_ratio` | Vector SFU conflict cycles as a fraction of total AI Vector Core cycles. Includes the sum of four SFU conflict counters. |
| `aiv_vec_wait_ratio` | Vector wait cycles as a fraction of total AI Vector Core cycles. |
| `aiv_mte2_wait_ratio` | MTE2 wait cycles as a fraction of total AI Vector Core cycles. |
| `aiv_mte3_wait_ratio` | MTE3 wait cycles as a fraction of total AI Vector Core cycles. |

## summary (Section summaries and operator information)

`summary.jsonl` uses JSON Lines, one object per line. The selected subset of the seven PMU Sections is emitted in alphabetical order, without duplicates, followed by OpInfoSummary. Pipeline-only collection also emits an operator summary when a Kernel collection result exists. Commands that execute no Kernel do not create a summary.

### Section summary records

The category is ArithmeticUtilization, L2Cache, Memory, MemoryL0, MemoryUB, PipeUtilization, or ResourceConflictRatio. Fields use the corresponding CSV names and units, excluding block_id and sub_block_id. Each field is independently averaged over valid Task PMU records. Unavailable fields remain present and use the exact JSON string "NA"; valid zero remains a number. Counter averages have no decimal places; other finite metrics have six.

This averaging is across Task records, a separate level from the cumulative Block coverage of a single Task. Task summaries read Task PMU directly. They neither generate Task data by multiplying Block data by a Block count nor fall back to Block data when Task input is missing. Valid sample counts may differ between Section fields; summaries cannot reconstruct Block details.

### Relationship between Block and Task metrics

The relationships below require the same Core type, the same Task execution, matching collection scope, and a complete valid Block set. Let $C_b$ be Block cycles, $P_b$ the relevant Event count, $Q_b$ bytes, $F$ the frequency for that Core type in MHz, $t_b=C_b/F$ execution time in us, and $T$ Task Log duration in us. AIC and AIV are handled separately.

**The sums, weighted relationships, and equations describe equivalent statistical meanings under these conditions, not code that accumulates Block data to generate Task data.** The implementation reads cycles and Event counts directly from Task PMU, then computes Task metrics. Different replays, missing data, sampling scope, and display rounding can prevent exact equality between displayed values.

| Metric | Relationship from Blocks to one Task | Difference when comparing with CSV |
|---|---|---|
| Total cycles, instruction counts, and L2 access counts | $C_{\mathrm{task}}=\sum_b C_b$; additive Event counts satisfy $P_{\mathrm{task}}=\sum_b P_b$. | Cumulative totals, not Block averages. |
| Execution time and pipeline active time | At the same frequency, $C_{\mathrm{task}}/F=\sum_b t_b$; active time is $\sum_b P_b/F$. | Cumulative Core time, not end-to-end Task duration. |
| Data volumes | For linear conversions, $Q_{\mathrm{task}}=\sum_b Q_b$; KB uses bytes divided by `1024`. | Fields with nonlinear adjustments such as clamping or rounding use their original formulas; derived values are not necessarily additive. |
| Active, wait, and conflict ratios | $\frac{\sum_b P_b}{\sum_b C_b}$, a cycle-weighted ratio. | Not an arithmetic mean of Block ratios; combine numerator Events as defined for each field. |
| L2 hit rates and I-cache miss rates | L2: $100\times\frac{\sum_b \mathrm{hit}_b}{\sum_b \mathrm{access}_b}$; I-cache: $\frac{\sum_b \mathrm{miss}_b}{\sum_b \mathrm{access}_b}$. | Access-weighted rates. I-cache does not multiply by `100`; zero denominators follow the corresponding field rules. |
| Bandwidth using cumulative execution time | $\frac{\sum_b Q_b\times10^6}{1024^3\times\sum_b t_b}$, with the output label GB/s. | An execution-time-weighted Block bandwidth, not a sum of bandwidths. |
| Active bandwidth | $\frac{\sum_b Q_b\times10^6}{1024^3\times\sum_b \mathrm{active\_time}_b}$. | Weighted by active time; nonlinear numerator adjustments still use the original formula. |
| Bandwidth using a common Task duration | $\frac{Q_{\mathrm{task}}\times10^6}{1024^3\times T}$. | Equals the sum of Block bandwidths only when the CSV rows also use the same `T` and data volumes are additive. |
| Bandwidth utilization within a Section | Compute Task bandwidth first, then apply the corresponding theoretical bandwidth and clipping rule. | Do not directly average or sum Block utilization values. OpInfoSummary GM utilization has its own formula below. |

Bandwidth fields in Memory, MemoryL0, and MemoryUB that use operation duration use Task duration when both AIC/AIV data and valid Task Logs are present; otherwise, they use cycles and frequency for the corresponding Core type. PipeUtilization active bandwidth uses pipeline active time. The summary prefers actual Current frequency, falls back to Rated frequency, and never substitutes CSV defaults when both are missing. Compare matching time and bandwidth fields only after checking that their effective frequencies and time denominators agree.

For example, two Blocks with cycles `1000, 3000` and active cycles `100, 1500` give Task cycles `4000`, active cycles `1600`, and ratio `0.4`. The arithmetic mean of the Block ratios is `0.3` and cannot replace the Task ratio. When multiple Task records exist, the summary then averages valid Task values independently for each field. Fields without an AIC/AIV prefix also use their applicable records; do not unconditionally treat the sum of both Core types as the final summary value. Calculations use unrounded inputs, so recomputing from rounded CSV values may differ in the last digits.

### OpInfoSummary fields

Numeric fields accept a number or the exact string "NA". Integer metadata accepts an integer or NA; missing text also uses NA. FP16 theoretical TFLOPS values have two decimal places; other derived floating-point values have six.

| Field | JSON type | Description |
|---|---|---|
| `category` | string | Always OpInfoSummary. |
| `Op Name` | string/NA | Readable Kernel name, with C++ names demangled when possible. |
| `Op Type` | string/NA | cube, vector, or mix. Prefer launch ratio/type metadata; use Task PMU only when metadata is unavailable. |
| `Task Duration(us)` | number/NA | Median valid Task duration from paired start/end records. |
| `Block Dim` | integer/NA | Launch logical Block count: Cube for mix, the corresponding Core type for a pure Kernel. |
| `Mix Block Dim` | integer/NA | Vector logical count for mix: AIV ratio times Block Dim. NA for non-mix or missing/invalid dimensions. |
| `Device Id` | integer/NA | Logical launch Device ID. |
| `Pid` | integer | Target process ID. |
| `Current Freq` | number/NA | Effective Core frequency used for calculations, in MHz. Prefer actual Current frequency, then valid Rated frequency. NA if neither is valid or the two effective mix frequencies differ; each Core type's metrics still use its own effective frequency. |
| `Rated Freq` | number/NA | Rated frequency in MHz; NA when unavailable or mix sides differ. |
| `aic_flops_theoretical` | number/NA | Hardware Cube count × 8192 × effective AIC frequency(MHz) / 10^6; FP16 TFLOPS, two decimal places. |
| `aiv_flops_theoretical` | number/NA | Hardware Vector count × 256 × effective AIV frequency(MHz) / 10^6; FP16 TFLOPS, two decimal places. |
| `aic_flops` | number/NA | mean_task(aic_total_cycles) × mean_task(aic_cube_fp_ratio) × 8192 / (TaskDuration(us) × 10^6). |
| `aiv_flops` | number/NA | mean_task(aiv_total_cycles) × mean_task(aiv_vec_ratio) × 256 / (TaskDuration(us) × 10^6). |
| `aicore_parallel_utilization` | number/NA | Sum of logical object execution times / (logical object count × TaskDuration) × 100, in percent. |
| `aicore_parallel_balance` | number/NA | Mean logical object execution time / maximum execution time × 100, in percent. Does not require Task duration. |
| `aicore_gm_bw_theoretical(GB/s)` | integer/NA | 1600 GB/s when Memory is selected, otherwise NA. |
| `aicore_gm_read_bw(GB/s)` | number/NA | Sum of available AIC/AIV main-memory read bandwidths from the Task Memory summary. |
| `aicore_gm_write_bw(GB/s)` | number/NA | Sum of available AIC/AIV main-memory write bandwidths from the Task Memory summary. |
| `aicore_gm_bw_usage_rate(%)` | number/NA | (Summary read bandwidth + summary write bandwidth) / 1600 × 100. NA if either is missing. |

Parallel metrics require the complete logical object set, identified by (blockId, subBlockId, coreType), ignoring physical coreId changes. Each time is total cycles divided by the effective frequency for that side. Missing records, conflicting duplicate cycles, or missing frequency make the metrics unavailable; the denominator is never reduced to the received row count. Mix preserves separate AIC/AIV times. All-zero times produce utilization 0 and balance NA. Missing Task duration only prevents utilization.

Estimated TFLOPS use unrounded cycles and ratios averaged over the same valid Task record set for each side. They do not multiply by Block Dim or Mix Block Dim, and never fall back to Block data when Task input is missing. These are active-cycle estimates, not exact operation counts. Hardware core counts are used only for theoretical compute capacity; 36/72 cores at 1650 MHz produce 486.60/30.41 TFLOPS.

Each side prefers valid Current frequency, then valid Rated frequency; missing values do not use CSV defaults. The explicit NPU_COMPUTE_FREQUENCY_MHZ override remains supported. The tool currently supports 950 only: ratios {1,0}/{0,1} identify pure cube/vector, while {1,1}/{1,2} identify mix. Mix Block Dim is AIV ratio times launch Block Dim, representing the logical Vector count.

## HardwareInfo (host and device information)

HardwareInfo.jsonl contains host and device information recorded during collection, including the device model, Core counts, frequencies, and memory capacities. See the field descriptions in the following tables for details.

`HardwareInfo.jsonl` is an environment snapshot with one JSON object per line. It contains five categories in this order: Host Info, Device Info, CPU Information, AI Core Information, Memory Information.

Total HBM capacity comes from the platform API; NPU count is the Runtime-visible device count. This is not a separate snapshot for every Kernel Device. Check `Device Id` in the summary when comparing devices. Failed queries may leave 0 or an empty string; these are not necessarily real zero values. Capacities have at most two decimal places with trailing zeros removed; frequencies and counts are integers.

### Host Info

| Field | JSON type | Description |
|---|---|---|
| `category` | string | Always `Host Info`. |
| `cpu physical count` | integer | Number of physical processor packages associated with online host CPUs; not physical Core count. |
| `cpu logical count` | integer | Configured host logical CPU count; may differ from online CPUs or application threads. |
| `memory total size(MB)` | number | Total host physical memory, in MB; not free memory or target-process usage. |
| `disk total size(GB)` | number | Total capacity of the filesystem at the queried path, in GB; not free space. |

### Device Info

| Field | JSON type | Description |
|---|---|---|
| `category` | string | Always `Device Info`. |
| `npu count` | integer | Runtime-visible NPU count, not the number used by this Kernel. |
| `chip info` | string | SoC name followed by a non-empty chip version, separated by a space. |
| `arch info` | string | NPU architecture identifier as a string, for example 3510. |

### CPU Information

| Field | JSON type | Description |
|---|---|---|
| `category` | string | Always `CPU Information`. |
| `control cpu count` | integer | Number of device-side Control CPU Cores. |
| `ai cpu count` | integer | Number of device-side AI CPU Cores. |
| `ai cpu frequency(MHZ)` | integer | Current device AI CPU frequency, in MHz. |

### AI Core Information

| Field | JSON type | Description |
|---|---|---|
| `category` | string | Always `AI Core Information`. |
| `ai core count` | integer | AI Core count as reported by the device API. |
| `ai cube count` | integer | Number of device Cube Cores. |
| `ai vector count` | integer | Number of device Vector Cores. |
| `ai cube frequency(MHZ)` | integer | Cube Core frequency in MHz: current frequency first, fixed frequency on query failure, then 1650 as the final fallback. |
| `ai vector frequency(MHZ)` | integer | Vector Core frequency in MHz: current frequency first, fixed frequency on query failure, then 1650 as the final fallback. |

### Memory Information

| Field | JSON type | Description |
|---|---|---|
| `category` | string | Always `Memory Information`. |
| `hbm total(MB)` | number | Total HBM capacity reported by the platform API, in MB. |
| `hbm used(MB)` | number | Used HBM capacity obtained by subtracting free memory from Runtime-allocatable memory, in MB. |
| `hbm frequency(MHZ)` | integer | HBM frequency returned by the device API, in MHz. |

## Pipeline (pipeline timeline)

PipeTrace.json contains busy intervals of sampled pipelines. Use it to examine busy times and overlapping intervals of sampled pipelines within the same replay. See the field descriptions in the following tables for details.

`PipeTrace.json` uses the Trace Event format. Each event represents a complete interval from the start to the end of pipeline activity. Event start times are relative to the earliest valid sample timestamp in the current replay.

### Top-level fields

| Field | JSON type | Description |
|---|---|---|
| `displayTimeUnit` | string | Always `ns`, the viewer display unit. It does not change the us unit of `ts` or `dur`. |
| `profilingType` | string | Always `op`. |
| `schemaVersion` | integer | Current schema version: 1. |
| `traceEvents` | array | Array of complete pipeline interval events; may be empty. |

### Event fields

| Field | JSON type | Description |
|---|---|---|
| `cname` | string | Color name for the pipeline track, listed below. |
| `dur` | number | Busy-interval duration in us, finite and non-negative. |
| `name` | string | Pipeline name: SCALAR, VECTOR, CUBE, MTE1, MTE2, MTE3, or FIXP. |
| `ph` | string | Always `X`, denoting a complete interval event. |
| `pid` | string | Source and Core track identifier, as described below; not an operating-system process ID. |
| `tid` | string | Pipeline track identifier; identical to `name`. |
| `ts` | number | Interval start relative to the Replay time origin, in us, finite and non-negative. |

### Pipelines and tracks

| Pipeline | `cname` | Meaning |
|---|---|---|
| SCALAR | startup | Scalar pipeline busy interval. |
| VECTOR | rail_idle | Vector pipeline busy interval. |
| CUBE | rail_response | Cube pipeline busy interval. |
| MTE1 | thread_state_iowait | MTE1 transfer pipeline busy interval. |
| MTE2 | yellow | MTE2 transfer pipeline busy interval. |
| MTE3 | rail_animation | MTE3 transfer pipeline busy interval. |
| FIXP | thread_state_unknown | Fixpipe pipeline busy interval. |

For a single source fragment, `pid` is `group<group_id>.<core>`. When multiple source fragments are merged, it becomes:

```text
process<process_ordinal>.result<result_sequence>.device<device_id>.replay<replay_id>.group<group_id>.<core>
```

The process ordinal is the sorted source-directory index; result sequence, device ID, and replay ID distinguish collection sources. `core` is `cubecore`, `veccore0`, or `veccore1`. A track is identified by `(pid, tid)`.

`group_id` ranges from 0 to 5, for at most six sampled Groups. It is not a logical Block or physical Core ID. Even when the Kernel uses all Cores, at most six Groups are shown; track count cannot determine how many Cores the Kernel used. Block identifiers are not exported in events.

An event ends at its start time plus its duration; array order is not time order. Tracks can overlap, and merging replays does not create a shared continuous time axis. Only busy intervals with both endpoints are written; incomplete intervals are skipped. An empty array means there are no complete intervals. Summing `dur` does not directly give Kernel execution time.
