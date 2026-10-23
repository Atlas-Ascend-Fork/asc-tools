# npu-compute性能数据

本文说明各Section CSV、`summary.jsonl`和`HardwareInfo.jsonl`的字段含义与计算方法。CSV展示Block/sub-block明细；summary包含所选Section的Task级指标及`OpInfoSummary`算子概览；HardwareInfo记录主机和设备信息。

| 输出内容 | 数据范围 | 与其他数据的关系 |
|---|---|---|
| Section CSV | Block/sub-block | 各逻辑执行对象的计数、执行时间及派生指标。 |
| summary的Section记录 | Task | 周期、计数和数据量对应累计；比例、命中率和带宽按累计基础数据及相应分母重算，不能统一理解为Block均值。 |
| `OpInfoSummary` | 当前Kernel | 组合Task指标、Block并行统计、运行时元数据和硬件能力信息。 |
| `HardwareInfo.jsonl` | 采集环境 | 提供主机及设备信息，与Kernel的启动逻辑数量不同。 |

同名字段的单位和基础公式一致，但统计范围、频率或时间分母可能不同。CSV与summary的对应关系见下文summary章节。

## ArithmeticUtilization（Cube及Vector指令占比和数量）

Cube及Vector类型指令的活跃周期占比和Cube指令条数保存在ArithmeticUtilization.csv中，可用于查看各类计算指令的活跃程度和Cube指令的执行数量。详情介绍请参见下表中的字段说明。

NA表示当前Core不适用或所需数据不可用，不表示数值零。

| 字段名 | 字段解释 |
|---|---|
| `block_id` | 当前数据行所属的逻辑Block编号。 |
| `sub_block_id` | 当前Block内的Core类型和Sub-block编号。AI Cube Core显示为cubeN，AI Vector Core显示为vectorN，其中N为Sub-block编号，不是物理核编号。 |
| `aic_time(us)` | 当前数据行对应AI Cube Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aic_total_cycles` | 当前数据行对应AI Cube Core执行的总周期数。 |
| `aic_cube_ratio` | AIC Cube活跃周期占AIC总周期的比值。 |
| `aic_cube_fp_ratio` | AIC Cube浮点指令活跃周期占AIC总周期的比值。 |
| `aic_cube_int_ratio` | AIC Cube整数指令活跃周期占AIC总周期的比值。 |
| `aic_cube_total_instr_number` | AIC Cube执行的总指令条数。 |
| `aic_cube_fp_instr_number` | AIC Cube执行的浮点指令条数。 |
| `aic_cube_int_instr_number` | AIC Cube执行的整数指令条数。 |
| `aiv_time(us)` | 当前数据行对应AI Vector Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aiv_total_cycles` | 当前数据行对应AI Vector Core执行的总周期数。 |
| `aiv_vec_ratio` | AIV Vector活跃周期占AIV总周期的比值。 |
| `aiv_vec_sfu_ratio` | AIV Vector SFU指令活跃周期占AIV总周期的比值。 |
| `aiv_simd_vf_ratio` | AIV SIMD VF指令发射周期占AIV总周期的比值。 |
| `aiv_simt_vf_ratio` | AIV SIMT VF指令发射周期占AIV总周期的比值。 |
| `aiv_simd_vf_time` | AIV SIMD VF指令发射总时长，单位为 us。 |
| `aiv_simd_vf_ipc` | 单位周期平均AIV SIMD VF指令发射数。 |
| `aiv_simt_vf_time` | AIV SIMT VF指令发射总时长，单位为 us。 |
| `aiv_simt_vf_ipc` | 单位周期平均AIV SIMT VF指令发射数。 |

## L2Cache（L2 Cache访问计数和命中率）

L2 Cache读写计数和命中率保存在L2Cache.csv中，可用于查看AI Cube Core和AI Vector Core访问近端、远端L2 Cache时的命中、未命中及驱逐情况。详情介绍请参见下表中的字段说明。

NA表示当前Core不适用或所需数据不可用，不表示数值零。

| 字段名 | 字段解释 |
|---|---|
| `block_id` | 当前数据行所属的逻辑Block编号。 |
| `sub_block_id` | 当前Block内的Core类型和Sub-block编号。AI Cube Core显示为cubeN，AI Vector Core显示为vectorN，其中N为Sub-block编号，不是物理核编号。 |
| `aic_time(us)` | 当前数据行对应AI Cube Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aic_total_cycles` | 当前数据行对应AI Cube Core执行的总周期数。 |
| `aic_read_close_hit` | AIC读取近端L2 Cache时的命中次数。 |
| `aic_read_close_miss` | AIC读取近端L2 Cache时的未命中次数。 |
| `aic_read_close_victim` | AIC读取近端L2 Cache时记录的驱逐次数。 |
| `aic_read_far_hit` | AIC读取远端L2 Cache时的命中次数。 |
| `aic_read_far_miss` | AIC读取远端L2 Cache时的未命中次数。 |
| `aic_read_far_victim` | AIC读取远端L2 Cache时记录的驱逐次数。 |
| `aic_read_hit_rate(%)` | AIC读取L2 Cache的命中率，以百分比表示。命中次数包括近端和远端命中，总次数包括近端和远端的命中、未命中、驱逐次数；计数齐全且总次数为零时显示为0。 |
| `aic_write_close_hit` | AIC写入近端L2 Cache时的命中次数。 |
| `aic_write_close_miss` | AIC写入近端L2 Cache时的未命中次数。 |
| `aic_write_close_victim` | AIC写入近端L2 Cache时记录的驱逐次数。 |
| `aic_write_far_hit` | AIC写入远端L2 Cache时的命中次数。 |
| `aic_write_far_miss` | AIC写入远端L2 Cache时的未命中次数。 |
| `aic_write_far_victim` | AIC写入远端L2 Cache时记录的驱逐次数。 |
| `aic_write_hit_rate(%)` | AIC写入L2 Cache的命中率，以百分比表示。命中次数包括近端和远端命中，总次数包括近端和远端的命中、未命中、驱逐次数；计数齐全且总次数为零时显示为0。 |
| `aiv_time(us)` | 当前数据行对应AI Vector Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aiv_total_cycles` | 当前数据行对应AI Vector Core执行的总周期数。 |
| `aiv_read_close_hit` | AIV读取近端L2 Cache时的命中次数。 |
| `aiv_read_close_miss` | AIV读取近端L2 Cache时的未命中次数。 |
| `aiv_read_close_victim` | AIV读取近端L2 Cache时记录的驱逐次数。 |
| `aiv_read_far_hit` | AIV读取远端L2 Cache时的命中次数。 |
| `aiv_read_far_miss` | AIV读取远端L2 Cache时的未命中次数。 |
| `aiv_read_far_victim` | AIV读取远端L2 Cache时记录的驱逐次数。 |
| `aiv_read_hit_rate(%)` | AIV读取L2 Cache的命中率，以百分比表示。命中次数包括近端和远端命中，总次数包括近端和远端的命中、未命中、驱逐次数；计数齐全且总次数为零时显示为0。 |
| `aiv_write_close_hit` | AIV写入近端L2 Cache时的命中次数。 |
| `aiv_write_close_miss` | AIV写入近端L2 Cache时的未命中次数。 |
| `aiv_write_close_victim` | AIV写入近端L2 Cache时记录的驱逐次数。 |
| `aiv_write_far_hit` | AIV写入远端L2 Cache时的命中次数。 |
| `aiv_write_far_miss` | AIV写入远端L2 Cache时的未命中次数。 |
| `aiv_write_far_victim` | AIV写入远端L2 Cache时记录的驱逐次数。 |
| `aiv_write_hit_rate(%)` | AIV写入L2 Cache的命中率，以百分比表示。命中次数包括近端和远端命中，总次数包括近端和远端的命中、未命中、驱逐次数；计数齐全且总次数为零时显示为0。 |

## Memory（内存读写带宽和数据量）

主存、L1和UB相关的读写带宽、数据量及搬运指令信息保存在Memory.csv中，可用于查看主存读写和各数据通路的搬运情况。详情介绍请参见下表中的字段说明。

带宽按搬运数据量和执行时间计算，单位为GB/s。混合算子有有效Task耗时时使用该耗时，否则使用当前Core的执行时间。

NA表示当前Core不适用或所需数据不可用，不表示数值零。

| 字段名 | 字段解释 |
|---|---|
| `block_id` | 当前数据行所属的逻辑Block编号。 |
| `sub_block_id` | 当前Block内的Core类型和Sub-block编号。AI Cube Core显示为cubeN，AI Vector Core显示为vectorN，其中N为Sub-block编号，不是物理核编号。 |
| `aic_time(us)` | 当前数据行对应AI Cube Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aic_total_cycles` | 当前数据行对应AI Cube Core执行的总周期数。 |
| `aic_l1_read_bw(GB/s)` | AIC从L1读取数据的平均带宽，单位为GB/s。 |
| `aic_l1_write_bw(GB/s)` | AIC向L1写入数据的平均带宽，单位为GB/s。 |
| `aic_main_mem_read_bw(GB/s)` | AIC主存读取平均带宽，单位为GB/s。 |
| `aic_main_mem_write_bw(GB/s)` | AIC主存写入平均带宽，单位为GB/s。 |
| `aic_mte1_instructions` | AIC执行的MTE1指令条数。 |
| `aic_mte1_ratio` | AIC MTE1活跃周期占总周期的比值。 |
| `aic_mte2_instructions` | AIC执行的MTE2指令条数。 |
| `aic_mte2_ratio` | AIC MTE2活跃周期占总周期的比值。 |
| `aic_mte3_instructions` | AIC执行的MTE3指令条数。 |
| `aic_mte3_ratio` | AIC MTE3活跃周期占总周期的比值。 |
| `aiv_time(us)` | 当前数据行对应AI Vector Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aiv_total_cycles` | 当前数据行对应AI Vector Core执行的总周期数。 |
| `aiv_ub_to_gm_bw(GB/s)` | 从UB搬运到GM的带宽，单位为GB/s。当前所需插桩数据未提供，固定显示为NA。 |
| `aiv_gm_to_ub_bw(GB/s)` | AIV GM到UB相关平均带宽，单位为GB/s。 |
| `aiv_main_mem_read_bw(GB/s)` | AIV主存读取平均带宽，单位为GB/s。 |
| `aiv_main_mem_write_bw(GB/s)` | AIV主存写入平均带宽，单位为GB/s。 |
| `aiv_mte2_instructions` | AIV执行的MTE2指令条数。 |
| `aiv_mte2_ratio` | AIV MTE2活跃周期占总周期的比值。 |
| `aiv_mte3_instructions` | AIV执行的MTE3指令条数。 |
| `aiv_mte3_ratio` | AIV MTE3活跃周期占总周期的比值。 |
| `read_main_memory_datas(KB)` | 当前数据行对应Core从主存读取的数据量，单位为KB。 |
| `write_main_memory_datas(KB)` | 当前数据行对应Core向主存写入的数据量，单位为KB。 |
| `GM_to_L1_datas(KB)` | 从GM搬运到L1的数据量，单位为KB。 |
| `GM_to_L1_bw_usage_rate(%)` | GM到L1通路平均带宽占配置最大带宽的百分比，最高显示为100%。 |
| `L0C_to_L1_datas(KB)` | 从L0C搬运到L1的数据量，单位为KB。 |
| `L0C_to_L1_bw_usage_rate(%)` | L0C到L1通路平均带宽占配置最大带宽的百分比，最高显示为100%。 |
| `L0C_to_GM_datas(KB)` | 从L0C搬运到GM的数据量，单位为KB。 |
| `L0C_to_GM_bw_usage_rate(%)` | L0C到GM通路平均带宽占配置最大带宽的百分比，最高显示为100%。 |
| `GM_to_UB_datas(KB)` | 从GM搬运到UB的数据量，单位为KB。 |
| `GM_to_UB_bw_usage_rate(%)` | GM到UB通路平均带宽占配置最大带宽的百分比，最高显示为100%。 |
| `UB_to_GM_datas(KB)` | 从UB搬运到GM的数据量，单位为KB。当前所需插桩数据未提供，固定显示为NA。 |
| `UB_to_GM_bw_usage_rate(%)` | UB到GM通路的带宽利用率，以百分比表示。当前所需插桩数据未提供，固定显示为NA。 |

GM到UB的数据量使用主存读取计数扣除两类DCache未命中相关读取计数后计算，扣除结果小于零时按零处理。GM到L1和L0C到GM分别使用AIC主存读取和写入计数计算。

## MemoryL0（L0读写带宽）

L0A、L0B、L0C与Cube或L1之间的读写带宽保存在MemoryL0.csv中，可用于查看各数据通路的平均带宽。详情介绍请参见下表中的字段说明。

带宽字段适用于AI Cube Core。带宽按搬运数据量和执行时间计算，单位为GB/s。混合算子有有效Task耗时时使用该耗时，否则使用当前Core的执行时间。

NA表示当前Core不适用或所需数据不可用，不表示数值零。

| 字段名 | 字段解释 |
|---|---|
| `block_id` | 当前数据行所属的逻辑Block编号。 |
| `sub_block_id` | 当前Block内的Core类型和Sub-block编号。AI Cube Core显示为cubeN，AI Vector Core显示为vectorN，其中N为Sub-block编号，不是物理核编号。 |
| `aic_time(us)` | 当前数据行对应AI Cube Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aic_total_cycles` | 当前数据行对应AI Cube Core执行的总周期数。 |
| `aic_l0a_read_bw(GB/s)` | L0A到Cube方向的读取带宽，单位为GB/s。 |
| `aic_l0a_write_bw(GB/s)` | L1到L0A方向的写入带宽，单位为GB/s。 |
| `aic_l0b_read_bw(GB/s)` | L0B到Cube方向的读取带宽，单位为GB/s。 |
| `aic_l0b_write_bw(GB/s)` | L1到L0B方向的写入带宽，单位为GB/s。 |
| `aic_l0c_read_bw_cube(GB/s)` | L0C到Cube方向的读取带宽，单位为GB/s。 |
| `aic_l0c_write_bw_cube(GB/s)` | Cube到L0C方向的写入带宽，单位为GB/s。 |
| `aiv_time(us)` | 当前数据行对应AI Vector Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aiv_total_cycles` | 当前数据行对应AI Vector Core执行的总周期数。 |

## MemoryUB（UB读写带宽）

UB与Vector、GM之间的读写带宽保存在MemoryUB.csv中，可用于查看Vector访问UB以及GM向UB搬入数据的平均带宽。详情介绍请参见下表中的字段说明。

带宽字段适用于AI Vector Core。带宽按搬运数据量和执行时间计算，单位为GB/s。混合算子有有效Task耗时时使用该耗时，否则使用当前Core的执行时间。

NA表示当前Core不适用或所需数据不可用，不表示数值零。

| 字段名 | 字段解释 |
|---|---|
| `block_id` | 当前数据行所属的逻辑Block编号。 |
| `sub_block_id` | 当前Block内的Core类型和Sub-block编号。AI Cube Core显示为cubeN，AI Vector Core显示为vectorN，其中N为Sub-block编号，不是物理核编号。 |
| `aic_time(us)` | 当前数据行对应AI Cube Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aic_total_cycles` | 当前数据行对应AI Cube Core执行的总周期数。 |
| `aiv_time(us)` | 当前数据行对应AI Vector Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aiv_total_cycles` | 当前数据行对应AI Vector Core执行的总周期数。 |
| `aiv_ub_read_bw_vector(GB/s)` | UB到Vector方向的数据读取带宽，单位为GB/s。 |
| `aiv_ub_write_bw_vector(GB/s)` | Vector到UB方向的数据写入带宽，单位为GB/s。 |
| `aiv_ub_read_bw_gm(GB/s)` | 从UB读取数据并搬往GM的带宽，单位为GB/s。当前所需插桩数据未提供，固定显示为NA。 |
| `aiv_ub_write_bw_gm(GB/s)` | 代表GM向UB写入数据的平均带宽，单位为GB/s。 |

## PipeUtilization（计算和搬运单元耗时占比）

计算和搬运单元的活跃时间、周期占比、活跃带宽及指令缓存未命中率保存在PipeUtilization.csv中。可用于查看各流水的活跃时间和周期占比。详情介绍请参见下表中的字段说明。

活跃带宽使用对应流水实际活跃时间计算。部分带宽还依赖Memory或MemoryUB提供的数据，单独采集本项时可能显示为NA。

NA表示当前Core不适用或所需数据不可用，不表示数值零。

| 字段名 | 字段解释 |
|---|---|
| `block_id` | 当前数据行所属的逻辑Block编号。 |
| `sub_block_id` | 当前Block内的Core类型和Sub-block编号。AI Cube Core显示为cubeN，AI Vector Core显示为vectorN，其中N为Sub-block编号，不是物理核编号。 |
| `aic_time(us)` | 当前数据行对应AI Cube Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aic_total_cycles` | 当前数据行对应AI Cube Core执行的总周期数。 |
| `aic_cube_time(us)` | Cube流水的活跃时间，单位为us。 |
| `aic_cube_ratio` | Cube活跃周期占AIC总周期的比值。 |
| `aic_scalar_time(us)` | AIC Scalar流水的活跃时间，单位为us。 |
| `aic_scalar_ratio` | AIC Scalar活跃周期占总周期的比值。 |
| `aic_mte1_time(us)` | AIC MTE1流水的活跃时间，单位为us。 |
| `aic_mte1_ratio` | AIC MTE1活跃周期占总周期的比值。 |
| `aic_mte1_active_bw(GB/s)` | AIC MTE1活跃期间的L1读取带宽，单位为GB/s。 |
| `aic_mte2_time(us)` | AIC MTE2流水的活跃时间，单位为us。 |
| `aic_mte2_ratio` | AIC MTE2活跃周期占总周期的比值。 |
| `aic_mte2_active_bw(GB/s)` | AIC MTE2活跃期间的主存读取带宽，单位为GB/s。 |
| `aic_mte3_time(us)` | AIC MTE3流水的活跃时间，单位为us。 |
| `aic_mte3_ratio` | AIC MTE3活跃周期占总周期的比值。 |
| `aic_mte3_active_bw(GB/s)` | 代表AIC MTE3活跃期间写入L1的相关带宽，使用扣除其他通路后的L1写入计数计算，单位为GB/s。 |
| `aic_fixpipe_time(us)` | AIC Fixpipe流水的活跃时间，单位为us。 |
| `aic_fixpipe_ratio` | AIC Fixpipe活跃周期占总周期的比值。 |
| `aic_fixpipe_active_bw(GB/s)` | AIC Fixpipe活跃期间的相关数据带宽，单位为GB/s。 |
| `aic_icache_miss_rate` | AIC指令缓存未命中次数与访问总次数的比值。 |
| `aiv_time(us)` | 当前数据行对应AI Vector Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aiv_total_cycles` | 当前数据行对应AI Vector Core执行的总周期数。 |
| `aiv_vec_time(us)` | Vector流水的活跃时间，单位为us。 |
| `aiv_vec_ratio` | Vector活跃周期占AIV总周期的比值。 |
| `aiv_scalar_time(us)` | AIV Scalar流水的活跃时间，单位为us。 |
| `aiv_scalar_ratio` | AIV Scalar活跃周期占总周期的比值。 |
| `aiv_mte2_time(us)` | AIV MTE2流水的活跃时间，单位为us。 |
| `aiv_mte2_ratio` | AIV MTE2活跃周期占总周期的比值。 |
| `aiv_mte2_active_bw(GB/s)` | AIV MTE2活跃期间的GM到UB相关带宽，单位为GB/s。 |
| `aiv_mte3_time(us)` | AIV MTE3流水的活跃时间，单位为us。 |
| `aiv_mte3_ratio` | AIV MTE3活跃周期占总周期的比值。 |
| `aiv_mte3_active_bw(GB/s)` | AIV MTE3活跃期间的带宽，单位为GB/s。当前所需插桩数据未提供，固定显示为NA。 |
| `aiv_icache_miss_rate` | AIV指令缓存未命中次数与访问总次数的比值。 |

## ResourceConflictRatio（资源冲突和等待占比）

计算和搬运单元的等待周期占比、Vector资源冲突周期占比保存在ResourceConflictRatio.csv中，可用于查看各单元的等待情况以及Vector资源冲突情况。详情介绍请参见下表中的字段说明。

NA表示当前Core不适用或所需数据不可用，不表示数值零。

| 字段名 | 字段解释 |
|---|---|
| `block_id` | 当前数据行所属的逻辑Block编号。 |
| `sub_block_id` | 当前Block内的Core类型和Sub-block编号。AI Cube Core显示为cubeN，AI Vector Core显示为vectorN，其中N为Sub-block编号，不是物理核编号。 |
| `aic_time(us)` | 当前数据行对应AI Cube Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aic_total_cycles` | 当前数据行对应AI Cube Core执行的总周期数。 |
| `aic_cube_wait_ratio` | AIC Cube等待周期占AIC总周期的比值。 |
| `aic_mte1_wait_ratio` | AIC MTE1等待周期占AIC总周期的比值。 |
| `aic_mte2_wait_ratio` | AIC MTE2等待周期占AIC总周期的比值。 |
| `aic_mte3_wait_ratio` | AIC MTE3等待周期占AIC总周期的比值。 |
| `aiv_time(us)` | 当前数据行对应AI Vector Core的执行时间，根据总周期数和该Core频率换算，单位为us。 |
| `aiv_total_cycles` | 当前数据行对应AI Vector Core执行的总周期数。 |
| `aiv_vec_stu_cflt_ratio` | AIV Vector STU冲突周期占AIV总周期的比值。 |
| `aiv_vec_ldu_cflt_ratio` | AIV Vector LDU冲突周期占AIV总周期的比值。 |
| `aiv_vec_sfu_cflt_ratio` | AIV Vector SFU冲突周期占AIV总周期的比值。冲突周期取四类SFU冲突计数之和。 |
| `aiv_vec_wait_ratio` | AIV Vector等待周期占AIV总周期的比值。 |
| `aiv_mte2_wait_ratio` | AIV MTE2等待周期占AIV总周期的比值。 |
| `aiv_mte3_wait_ratio` | AIV MTE3等待周期占AIV总周期的比值。 |

## summary（Section汇总和算子基础信息）

所选性能指标的Task汇总和算子基础信息保存在`summary.jsonl`中，每行一个JSON对象。七个PMU Section按名称字母升序输出所选子集，重复选择只输出一次；最后固定为`OpInfoSummary`。仅选择Pipeline时，有Kernel采集结果也生成概览。未执行Kernel时不凭空创建摘要。

### Section汇总记录

Section的`category`为对应名称：ArithmeticUtilization、L2Cache、Memory、MemoryL0、MemoryUB、PipeUtilization或ResourceConflictRatio。指标沿用对应CSV字段及单位，去除`block_id`和`sub_block_id`。每字段独立对有效Task PMU记录求算术平均；无有效值时保留字段并输出精确字符串`"NA"`，数值零保持number。

这里的平均发生在多条Task记录之间，与单次Task对Block数据的累计覆盖是两个层级。Task摘要直接使用Task PMU，不通过Block数据乘Block数量生成，缺Task输入时不从Block数据回退。计数均值不保留小数，其他有限指标保留六位；各字段可能使用不同有效样本数，不能从摘要还原Block明细。

### Block与Task指标的关系

以下关系针对同一侧、同一次Task、采集范围一致且完整有效的Block集合。$C_b$为Block周期，$P_b$为对应Event计数，$Q_b$为字节数，$F$为对应侧频率（MHz），$t_b=C_b/F$为执行时间（us），$T$为Task Log耗时（us）；AIC与AIV分别统计。

**统一说明：表中求和、加权和等式表示上述条件下统计含义上的等效关系，不表示当前代码累加Block数据生成Task数据。** 实现直接读取Task PMU的周期及Event计数，再计算Task指标。来自不同replay的记录、缺失数据、采样范围或显示舍入差异，都可能使显示值不严格相等。

| 指标 | Block到单次Task的关系 | 与CSV对照时的区别 |
|---|---|---|
| 总周期、指令数、L2访问计数 | $C_{\mathrm{task}}=\sum_b C_b$，可加Event计数$P_{\mathrm{task}}=\sum_b P_b$。 | 对应累计值，不是Block均值。 |
| 执行时间、流水线活跃时间 | 同频率时，$C_{\mathrm{task}}/F=\sum_b t_b$；活跃时间为$\sum_b P_b/F$。 | 是累计核时间，不能代替端到端Task耗时。 |
| 数据量 | 线性换算时$Q_{\mathrm{task}}=\sum_b Q_b$，KB按字节数除以`1024`。 | 含截零、取整等非线性修正的字段按原字段公式重算，不保证派生值可直接相加。 |
| 活跃、等待和冲突ratio | $\frac{\sum_b P_b}{\sum_b C_b}$，相当于按周期加权的比例。 | 不是各Block ratio的算术平均；多个分子Event按字段定义组合。 |
| L2命中率、I-cache miss rate | L2为$100\times\frac{\sum_b \mathrm{hit}_b}{\sum_b \mathrm{access}_b}$；I-cache为$\frac{\sum_b \mathrm{miss}_b}{\sum_b \mathrm{access}_b}$。 | 按访问量加权，I-cache不额外乘`100`；分母为零时按对应字段规则处理。 |
| 以累计执行时间为分母的带宽 | $\frac{\sum_b Q_b\times10^6}{1024^3\times\sum_b t_b}$，输出标签为GB/s。 | 相当于按执行时间加权的Block带宽，不是带宽之和。 |
| active bandwidth | $\frac{\sum_b Q_b\times10^6}{1024^3\times\sum_b \mathrm{active\_time}_b}$。 | 按活跃时间加权；非线性修正的分子仍按原公式重算。 |
| 以统一Task耗时为分母的带宽 | $\frac{Q_{\mathrm{task}}\times10^6}{1024^3\times T}$。 | 若各Block CSV也使用相同`T`且数据量可加，则对应这些Block带宽之和。 |
| Section内的带宽使用率 | 先计算Task带宽，再应用对应理论带宽和截断规则。 | 不直接平均或累加各Block使用率；OpInfoSummary的GM使用率见其独立公式。 |

Memory、MemoryL0、MemoryUB中使用operation duration的带宽字段，在同时有AIC/AIV数据且Task Log有效时以Task耗时为分母，否则按对应侧周期和频率计算；PipeUtilization的active bandwidth以流水线活跃时间为分母。summary优先实际当前频率，失效时回退对应额定频率，均缺失时不使用CSV默认频率。因此比较同名时间和带宽时，还需确认两者的有效频率和时间分母一致。

例如两个Block周期为`1000、3000`，活跃周期为`100、1500`，则Task周期为`4000`、活跃周期为`1600`，ratio为`0.4`；Block ratio的简单均值为`0.3`，不能代替Task ratio。存在多条Task记录时，summary再按每个字段的有效Task值求均值；无侧前缀的字段也按实际适用记录聚合，不无条件将AIC/AIV两侧之和当作最终摘要。计算使用未舍入输入，已舍入CSV值的复算可能存在末位误差。

### OpInfoSummary字段

数值字段只接受number或精确字符串`"NA"`，整数元数据只接受integer或NA；文本缺失也为NA。理论算力保留两位小数，其他派生数值保留六位。

| 字段名 | JSON类型 | 字段解释 |
|---|---|---|
| `category` | string | 记录类别。固定为OpInfoSummary；不可用条件：不允许为NA。 |
| `Op Name` | string/NA | 当前Kernel的可读名称。用于标识当前汇总对应的Kernel；不可用条件：未获得Kernel名称元数据。 |
| `Op Type` | string/NA | 优先由Kernel元数据配比判定的算子类型，缺失时使用类型查询或Task PMU。cube表示仅AIC，vector表示仅AIV，mix表示同时包含AIC和AIV；不可用条件：无可识别的Kernel类型元数据及Task PMU类型。 |
| `Task Duration(us)` | number/NA | 当前Kernel对应Task的执行时长。用于观察Kernel的整体执行时长；不可用条件：缺少有效Task Log时间区间。 |
| `Block Dim` | integer/NA | Task运行切分数量，即启动逻辑核数；Mix中可理解为Cube数量。cube对应AIC数量，vector对应AIV数量，mix对应主侧AIC数量；不可用条件：未获得Block Dim元数据。 |
| `Mix Block Dim` | integer/NA | Mix算子从AI处理器的逻辑Block数，可理解为Vector数量。仅 {1,1}/{1,2} 为Mix，值为AIV比例乘Block Dim；两种配比分别对应一倍、两倍Block Dim；不可用条件：非Mix、配比或维度缺失、零维度或乘法溢出。 |
| `Device Id` | integer/NA | 当前Kernel所在Device的逻辑编号。用于区分多Device场景中的采集对象；不可用条件：未获得Device元数据。 |
| `Pid` | integer | 执行当前Kernel的目标进程ID。用于关联目标进程；不可用条件：不允许为NA。 |
| `Current Freq` | number/NA | 计算指标时使用的有效Core频率，单位MHz。优先实际当前频率，不可用时回退对应额定频率；不可用条件：两者均无效，或混合Kernel的AIC与AIV有效频率不一致。各侧指标仍分别使用自身有效频率。 |
| `Rated Freq` | number/NA | 硬件信息中记录的额定Core频率。用于比较当前频率与额定频率；不可用条件：额定频率缺失，或混合Kernel的AIC与AIV额定频率不一致。 |
| `aic_flops_theoretical` | number/NA | Cube FP16理论算力。硬件Cube核数 × 8192 × 有效频率(MHz) / 10^6；保留两位小数，与逻辑Block数量及Section选择无关；不可用条件：对应硬件核数或有效频率不可用。 |
| `aiv_flops_theoretical` | number/NA | Vector FP16理论算力。硬件Vector核数 × 256 × 有效频率(MHz) / 10^6；保留两位小数，与逻辑Block数量及Section选择无关；不可用条件：对应硬件核数或有效频率不可用。 |
| `aic_flops` | number/NA | Cube FP16估算算力。mean_task(aic_total_cycles) × mean_task(aic_cube_fp_ratio) × 8192 / (TaskDuration(us) × 10^6)；同侧同一有效Task集合，不乘Block数量，不从Block数据回退；保留六位小数；不可用条件：对应侧不适用、缺少必需Task PMU输入、Task时长无效或输入为负/非有限。 |
| `aiv_flops` | number/NA | Vector FP16估算算力。mean_task(aiv_total_cycles) × mean_task(aiv_vec_ratio) × 256 / (TaskDuration(us) × 10^6)；同侧同一有效Task集合，不乘Block数量，不从Block数据回退；保留六位小数；不可用条件：对应侧不适用、缺少必需Task PMU输入、Task时长无效或输入为负/非有限。 |
| `aicore_parallel_utilization` | number/NA | 逻辑执行对象平均执行时间占Task时长的百分比。Σ Block执行时间 / (逻辑对象数量 × TaskDuration) × 100；mix保留AIC/AIV独立对象，数量为Block Dim + Mix Block Dim；不使用设备硬件核数或活跃ratio；不可用条件：缺少有效Task时长、元数据预期数量、完整Block执行记录或有效频率。 |
| `aicore_parallel_balance` | number/NA | 完整逻辑执行对象的平均时间与最大时间之比。mean(Block执行时间) / max(Block执行时间) × 100；mix合并各侧独立时间序列，不逐Block相加或取最大；不依赖Task时长；不可用条件：缺少完整Block执行记录、预期数量、有效频率或最大执行时间不大于零。 |
| `aicore_gm_bw_theoretical(GB/s)` | integer/NA | 计算GM带宽使用率时采用的理论带宽。当前值为1600 GB/s；不可用条件：未选择Memory。 |
| `aicore_gm_read_bw(GB/s)` | number/NA | Memory汇总中的AIC与AIV主存读取带宽之和。反映当前Kernel的汇总GM读取带宽；不可用条件：未选择Memory，或AIC与AIV主存读取带宽均不可用。 |
| `aicore_gm_write_bw(GB/s)` | number/NA | Memory汇总中的AIC与AIV主存写入带宽之和。反映当前Kernel的汇总GM写入带宽；不可用条件：未选择Memory，或AIC与AIV主存写入带宽均不可用。 |
| `aicore_gm_bw_usage_rate(%)` | number/NA | GM读写带宽之和占理论带宽的百分比。按 (读取带宽 + 写入带宽) / 1600 × 100计算；不可用条件：GM读取或写入带宽不可用。 |

并行指标的逻辑身份为`(blockId, subBlockId, coreType)`，忽略物理coreId变化；完整对象集合中的时间为总周期除以对应侧有效频率。缺行、冲突周期或无有效频率时不缩小分母。全零时间的利用率为0、均衡度为NA；缺Task时长仅使利用率不可用。mix保留两侧独立时间，不逐Block求和或取最大值。

算力采用FP16口径、TFLOPS单位，AIC每周期系数8192，AIV为256。估算仅使用同侧同一组有效Task PMU记录，周期和ratio先求均值再求乘积，不乘Block Dim或Mix Block Dim。缺Task数据时不能从Block数据回退。理论算力使用硬件核数，与启动逻辑数量不同；36/72核、1650 MHz对应486.60/30.41 TFLOPS。

各侧频率优先有效Current，其次有效Rated，均缺失时不用CSV默认频率。显式`NPU_COMPUTE_FREQUENCY_MHZ`仍可覆盖。当前仅支持950，合法配比`{1,0}`/`{0,1}`为cube/vector，`{1,1}`/`{1,2}`为mix。Mix Block Dim为AIV比例乘Block Dim，表示从侧Vector逻辑数量。

## HardwareInfo（主机和设备信息）

采集时的主机和设备信息保存在HardwareInfo.jsonl中，包括设备型号、Core数量、频率及内存容量等。详情介绍请参见下文各表中的字段说明。

`HardwareInfo.jsonl`为采集环境快照，每行一个JSON对象。固定包含以下五个category，顺序为Host Info、Device Info、CPU Information、AI Core Information、Memory Information。

HBM总容量来自平台接口，NPU数量为Runtime可见设备数。该文件并不是对每个Kernel实际Device分别生成的快照，跨设备比较时应结合summary中的`Device Id`。查询失败的字段可能保留0或空字符串，不能一律解释为真实零值。容量最多保留两位小数并去掉末尾零，频率和计数为整数；

### Host Info

| 字段名 | JSON类型 | 字段解释 |
|---|---|---|
| `category` | string | 固定为`Host Info`。 |
| `cpu physical count` | integer | 主机在线CPU所属的物理处理器数量，即Socket/Package数量，不是物理核数量。 |
| `cpu logical count` | integer | 主机配置的逻辑CPU数量，不一定等于当前在线CPU数量或程序线程数。 |
| `memory total size(MB)` | number | 主机物理内存总容量，单位为MB，不是空闲内存或目标程序占用量。 |
| `disk total size(GB)` | number | 工具查询路径所在文件系统的总容量，单位为GB，不是剩余空间。 |

### Device Info

| 字段名 | JSON类型 | 字段解释 |
|---|---|---|
| `category` | string | 固定为`Device Info`。 |
| `npu count` | integer | Runtime可见NPU设备数量，不表示本次Kernel使用数量。 |
| `chip info` | string | SoC名称与非空芯片版本用空格连接。 |
| `arch info` | string | 设备的NPU架构编号，以字符串表示，例如3510。 |

### CPU Information

| 字段名 | JSON类型 | 字段解释 |
|---|---|---|
| `category` | string | 固定为`CPU Information`。 |
| `control cpu count` | integer | 设备侧Control CPU核数。 |
| `ai cpu count` | integer | 设备侧AI CPU核数。 |
| `ai cpu frequency(MHZ)` | integer | 设备AI CPU当前频率，单位为MHz。 |

### AI Core Information

| 字段名 | JSON类型 | 字段解释 |
|---|---|---|
| `category` | string | 固定为`AI Core Information`。 |
| `ai core count` | integer | 设备接口报告的AI Core数量，按接口原值记录。 |
| `ai cube count` | integer | 设备Cube Core数量。 |
| `ai vector count` | integer | 设备Vector Core数量。 |
| `ai cube frequency(MHZ)` | integer | Cube Core频率，单位为MHz；优先当前频率，查询失败回退固定频率，再失败使用1650。 |
| `ai vector frequency(MHZ)` | integer | Vector Core频率，单位为MHz；优先当前频率，查询失败回退固定频率，再失败使用1650。 |

### Memory Information

| 字段名 | JSON类型 | 字段解释 |
|---|---|---|
| `category` | string | 固定为`Memory Information`。 |
| `hbm total(MB)` | number | 平台接口报告的HBM总容量，单位为MB。 |
| `hbm used(MB)` | number | Runtime可分配HBM总量减去空闲量后得到的已使用容量，单位为MB。 |
| `hbm frequency(MHZ)` | integer | 设备接口返回的HBM频率，单位为MHz。 |

## Pipeline（流水时间线）

采样流水的忙碌区间保存在PipeTrace.json中，可用于查看同一次重放中各采样流水的忙碌时间及区间重叠情况。详情介绍请参见下文各表中的字段说明。

`PipeTrace.json`采用Trace Event格式，每个事件记录一条流水从忙碌开始到忙碌结束的完整区间。事件开始时间以当前重放中最早的有效采样时间为起点。

### 顶层字段

| 字段名 | JSON类型 | 字段解释 |
|---|---|---|
| `displayTimeUnit` | string | 固定为`ns`，是查看器展示单位；不改变`ts`和`dur`的us数据单位。 |
| `profilingType` | string | 固定为`op`。 |
| `schemaVersion` | integer | 当前结构版本为1。 |
| `traceEvents` | array | 完整流水区间事件数组，允许为空。 |

### 事件字段

| 字段名 | JSON类型 | 字段解释 |
|---|---|---|
| `cname` | string | 流水轨道的颜色名称，见下表。 |
| `dur` | number | 忙碌区间持续时间，单位为us，有限且非负。 |
| `name` | string | 流水名称：SCALAR、VECTOR、CUBE、MTE1、MTE2、MTE3或FIXP。 |
| `ph` | string | 固定为`X`，表示完整区间事件。 |
| `pid` | string | 来源和Core轨道标识，格式见下文；此字段不是操作系统进程ID。 |
| `tid` | string | 流水轨道标识，与`name`相同。 |
| `ts` | number | 相对于该Replay时间原点的区间开始时间，单位为us，有限且非负。 |

### 流水和轨道

| 流水名称 | `cname` | 含义 |
|---|---|---|
| SCALAR | startup | 标量流水忙碌区间。 |
| VECTOR | rail_idle | Vector流水忙碌区间。 |
| CUBE | rail_response | Cube流水忙碌区间。 |
| MTE1 | thread_state_iowait | MTE1搬运流水忙碌区间。 |
| MTE2 | yellow | MTE2搬运流水忙碌区间。 |
| MTE3 | rail_animation | MTE3搬运流水忙碌区间。 |
| FIXP | thread_state_unknown | Fixpipe流水忙碌区间。 |

单个来源片段的`pid`为`group<group_id>.<core>`；多个来源片段合并时为：

```text
process<process_ordinal>.result<result_sequence>.device<device_id>.replay<replay_id>.group<group_id>.<core>
```

其中process ordinal是来源进程目录的排序序号，result sequence是结果序号，device id和replay id用于区分采集来源。`core`为`cubecore`、`veccore0`或`veccore1`。轨道由`(pid, tid)`共同确定。

`group_id`范围为0～5，最多6个采样Group；它不等于逻辑Block或物理Core编号。即使Kernel启用了全部Core，时间线也最多展示6个Group，不能根据轨道数推断实际启用核数。Block标识未写入事件。

事件结束时间为开始时间加持续时间，数组顺序不代表时间顺序。不同轨道可以重叠，多Replay之间也没有由此建立统一连续时间轴。只写入起止齐全的忙碌区间，缺少端点的区间会跳过；空数组仅表示没有完整区间。不同流水的忙碌时间可能重叠，累计忙碌时间不能直接等同于Kernel执行时长。
