# AI Core 异常特征与修复文档总索引

按 `SKILL.md` 确定的表象分区查找。对照**报错码**、**通过子例与失败子例的边界差异**及**源码特征词**，只打开命中的 **1~2 篇** `references/guides/*.md` 指南执行排查与修复。

## 一、P1 全局显存访问异常（`0x800000` / `0x8000` / `SMMU` / `errcode ∈ {1, 141}`）

| 报错码或运行表现 | 过/崩子例对比特征 | 源码特征词（直接搜索核对） | 直达专题修复指南 |
| --- | --- | --- | --- |
| `0x800000`、`0x8000`、`507015` | 常规设备侧正数分组/块表通过；传入 `[-1]` 负分组、超总行数分组、显式越界页表（测试期望报 `161002`）或主机侧裸缓冲（`host_placed`）时崩溃 | `groupList`、`group_list`、`stateBlockTable`、`GetPlacement`、`aclrtPointerGetAttributes` | `references/guides/host_pointer_and_control_tensor.md` |
| `0x800000`、`0x8000` | 合法序列与配对可选张量通过；非单调累积序列、`S1 > S2`、两序列元素个数不等、必填或可选张量传空指针（`nullptr`）时崩溃 | `actualSeqLengths`、`cu_seqlens`、`s1`、`s2`、`smooth_scales`、`sinks`、`CheckPtr` | `references/guides/host_seqlen_and_dim_contract.md` |
| `0x800000`、`0x8000`、`errcode=1` | 测试要求正常返回 `0`：负索引 `-1`/超 `numWeights` 词表下标、分页 `blockTable` 未用槽位 `-1`/越界、分组总覆盖行数 `< rowNum` 或 `bIndex > dimB` 尾行越界 | `embedding`、`numWeights`、`paddingIdx`、`blockTable`、`seqused`、`groupIdx`、`bIndex` | `references/guides/mem_window_overflow_and_index_clamp.md` |
| `0x800000`、`errcode=1`（`biu_l2_write_oob`） | 对齐形状通过，非整除尾块或大 `DataCopy` 步长越过输出张量分配末址 | `DataCopy`、`DataCopyPad`、`SetGlobalBuffer`、`tailLen`、`srcStride`、`dstStride` | `references/guides/mem_datacopy_and_pad_constraints.md` |
| `errcode=141`（`fixp_err_out_write_overflow`）、`fixp_biu_rdwr_resp` | 矩阵乘写回 GM 越界（须按 `SetAtomicAdd` 是否开启区分原子累加写越界与普通 `Fixpipe` 写越界） | `Fixpipe`、`FixpipeParamsV220`、`SetAtomicAdd`、`SetAtomicNone`、`dstStride`、`dstNdStride` | `references/guides/mem_fixpipe_and_cube_bounds.md` |
| `0x8000`、`0x10`（裸核加载崩溃） | 裸设备核函数直接用 `bisheng -c` 产出未链接对象即被 `binary_load` 加载执行 | `bisheng`、`--cce-aicore-only`、`ld.lld`、`aicorelinux`、`binary_load` | `references/guides/arch_compile_and_bare_kernel_link.md` |

## 二、P2 片上缓存越界（`0x4000000000000000` / `0x4000` / `0x200000000` / `0x1000000000000` / `errcode ∈ {24,26,42,47,48,93,103,124,125,154}`）

| 报错码或运行表现 | 过/崩子例对比特征 | 源码特征词（直接搜索核对） | 直达专题修复指南 |
| --- | --- | --- | --- |
| `0x4000000000000000`、`errcode ∈ {124, 154}` | `8192` 阶通过而 `8193` 阶报 UB 越界（超出算子固定单列缓冲规格上限） | `TILE_LENGTH`、`8192`、`ACL_ERROR_INVALID_PARAM`、`getrf`、`getri`、`infershape` | `references/guides/host_spec_bound_and_align_guard.md` |
| `0x4000000000000000`、`0x4000`、`errcode=154` | 常规尺寸通过，单维超 `65536`（如 `131073`）使 `idx * inLen` 发生 32 位有符号乘法溢出变负偏移 | `inLen`、`outLen`、`startIdx`、`endIdx`、`AdaptiveMaxPool`、`Pool` | `references/guides/mem_window_overflow_and_index_clamp.md` |
| `0x4000000000000000`、`0x4000`、`errcode=154` | 超宽列单行超 UB 容量、漏算 `int8`/双缓冲队列字节、末尾硬编码覆盖 `matmulTiling` 或核函数超大局部数组压栈 | `InitBuffer`、`AllocTensor`、`maxUbSize`、`numCol`、`matmulTiling`、`TBuf`、`TQue` | `references/guides/mem_ub_and_stack_bounds.md` |
| `errcode ∈ {124, 125}`（注意非 `exit_code=124`） | `DataCopyPad` 隐式 32B 行对齐写穿 UB，或普通 MTE 读写 UB 跨度超出 `LocalTensor` 分配 | `DataCopyPad`、`leftPadding`、`rightPadding`、`DataCopyExtParams`、`DataCopy` | `references/guides/mem_datacopy_and_pad_constraints.md` |
| `0x200000000`、`errcode ∈ {24, 26, 42, 47, 48, 93}` | `LoadData2D`、`LoadData3D` 或 `Nd2Nz` 读写跨度超出 L1 分配、首窗口越界或 `c0` 索引越界 | `LoadData2D`、`LoadData3D`、`CopyGm2Cbuf`、`Nd2Nz`、`c0`、`l1H`、`l1W` | `references/guides/mem_loaddata_2d_3d_constraints.md` |
| `0x1000000000000`、`0x1000000008000`、`0x4000000000`、`errcode=103` | `Mmad` 或 L1->L0 搬运跨出 L0A/L0B（64KB）或 L0C（128KB），或 `Fixpipe` 读写 L1/FBUF 越界 | `Mmad`、`LoadData`、`Fixpipe`、`srcStride`、`srcNdStride`、`cL0c`、`fbuf` | `references/guides/mem_fixpipe_and_cube_bounds.md` |

## 三、P3 指令非法或地址未对齐（`0x10` / `0x10000` / `0x1000` / `0x9e` / `0xa0` / `errcode ∈ {30,31,35,36,53,101,126}`）

| 报错码或运行表现 | 过/崩子例对比特征 | 源码特征词（直接搜索核对） | 直达专题修复指南 |
| --- | --- | --- | --- |
| `0x10000`、`0x1000`、`507035` | `N=64` 通过而非 8 倍数 `N=60` 报错；或 `headDim=4096` 通过而 `8192/16384` 使单核 UB 分片整除塌缩为 `0` | `% 8`、`headDim`、`ubFactor`、`GetWorkspaceSize`、`GRAPH_FAILED`、`infershape` | `references/guides/host_spec_bound_and_align_guard.md` |
| `0x10000`（`ADDR_MISALIGN`） | 变长序列 `actual_seq_qlen` / `cu_seqlens` 非单调产生负行长，无符号回绕后生成非对齐地址 | `actual_seq`、`cu_seqlens`、`metadata_check`、`inValidRowCount` | `references/guides/host_seqlen_and_dim_contract.md` |
| `0x10`（`illegal instruction`） | 编译 `--cce-aicore-arch` 与运行芯片不符，或裸设备核函数漏 `--cce-aicore-only` 与 `ld.lld -m aicorelinux -Ttext=0` | `--cce-aicore-arch`、`__NPU_ARCH__`、`dav-c220`、`--cce-aicore-only`、`ld.lld` | `references/guides/arch_compile_and_bare_kernel_link.md` |
| `0x10000`、`errcode ∈ {30, 31}` | 整除 `tailLen == 0` 仍调用 `DataCopy`（块长/块数为 0），或类型化指针偏移/原子写 GM 未满足 32B 对齐 | `DataCopy`、`blockLen`、`blockCount`、`SetAtomicAdd`、`align`、`% 32` | `references/guides/mem_datacopy_and_pad_constraints.md` |
| `0x1000`、`errcode=53` | Vector 计算掩码全 0、`repeatTimes == 0` 或 `repeatTimes > 255` | `SetMaskCount`、`SetVectorMask`、`repeatTimes`、`calCount` | `references/guides/mem_ub_and_stack_bounds.md` |
| `0x10000`、`0x1000`、`errcode ∈ {35, 36}` | `LoadData3D` 特征图/L1 尺寸非法、`channelSize` 非 C0 对齐、`strideW/H == 0` 或 `kStartPt/mStartPt` 非对齐越界 | `LoadData3D`、`channelSize`、`strideW`、`strideH`、`kStartPt`、`mStartPt` | `references/guides/mem_loaddata_2d_3d_constraints.md` |
| `0x9e`、`0xa0`、`errcode ∈ {101, 126}` | `Mmad` 维度为 0 或 L0C 偏移非 16 元素对齐；`Fixpipe` 非 `NZ2ND` 模式 `dstStride==0`、`unitFlag` 或 `isChannelSplit` 非法 | `Mmad`、`FixpipeParamsV220`、`unitFlag`、`isChannelSplit`、`dstStride`、`NZ2ND` | `references/guides/mem_fixpipe_and_cube_bounds.md` |

## 四、P4 核函数超时挂死（`507014` / `exit_code=124` / `rdwr_cflt` / `errcode ∈ {14, 39, 41}`）

| 报错码或运行表现 | 过/崩子例对比特征 | 源码特征词（直接搜索核对） | 直达专题修复指南 |
| --- | --- | --- | --- |
| `507014`、`exit_code=124` | 正数分组通过，`groupList=[-1]` 负值或主机侧未搬运张量导致核内无符号超大死循环超时 | `groupList`、`group_list`、`GetPlacement`、`aclrtPointerGetAttributes` | `references/guides/host_pointer_and_control_tensor.md` |
| `507014`、`exit_code=124`、`errcode ∈ {14, 39}` | `K=128` 窄块通过而 `K=256` 宽块死锁；或条件分支漏 `FreeTensor`/`DeQue`/`SetFlag`、部分核早退跳过 `SyncAll`、L0 读写未同步冲突 | `L1B_STAGES`、`Post`、`FwdH`、`SetFlag`、`WaitFlag`、`EnQue`、`DeQue`、`SyncAll` | `references/guides/sync_pipeline_and_wide_tile_deadlock.md` |
| `errcode=41`（伴随 `exit_code=124` 挂起） | `LoadData3D` 补齐后特征图仍小于卷积核尺寸（`H + 2*pad < filterH`），MTE1 窗口状态机挂起 | `LoadData3D`、`padList`、`padTop`、`filterH`、`filterW` | `references/guides/mem_loaddata_2d_3d_constraints.md` |

## 五、P5 数值异常（除零 / 非法定义域 / `NaN` / `Inf` / `errcode ∈ {3, 9}`）

| 报错码或运行表现 | 过/崩子例对比特征 | 源码特征词（直接搜索核对） | 直达专题修复指南 |
| --- | --- | --- | --- |
| `errcode ∈ {3, 9}`、输出含 `NaN` / `Inf` | 零除数属性、负数开方/非正数对数、`half` 归约超 `65504` 溢出、`Mmad` 首拍未清零或累加型 `workspace` 未清零 | `Div`、`Sqrt`、`Ln`、`Exp`、`Cast`、`cmatrixInitVal`、`eps`、`GetSysWorkSpacePtr` | `references/guides/numeric_domain_and_nan_inf.md` |
| 输出含 `NaN` 或尾块数值失真 | 非 64 对齐尾块归约混入 UB 尾部脏 `NaN`，或标量写 UB 后未刷缓存直接做 Vector 运算 | `DoSum`、`ReduceSum`、`Duplicate`、`PipeBarrier`、`DcciUBImpl` | `references/guides/sync_cache_coherency_and_norm_tail.md` |

## 六、P6 无硬件报错的静默结果或返回码异常（`exit_code=0` 精度失真 / 越界索引 / 返回码不符）

| 报错码或运行表现 | 过/崩子例对比特征 | 源码特征词（直接搜索核对） | 直达专题修复指南 |
| --- | --- | --- | --- |
| `exit_code=0` 精度崩坏或连续多轮后轮出错 | 恰好卡在临界尺寸（如 `K == 2048`、`tail == 0`）落入错误模板，或单跑正常、同进程多形状连跑污染 `tilingKey` | `>` vs `>=`、`2048`、`static`、全局变量、`tilingKey`、`matmulTiling` | `references/guides/host_tiling_state_and_threshold.md` |
| `exit_code=0` 精度失真或副输出错误 | 非对齐尾块归约偏差、标量写 UB/GM 后 Vector/原子加读到旧值、或 `RMS_NORM` 模式漏初始化 `mean` 副输出 | `DcciUBImpl`、`DataCacheCleanAndInvalid`、`PipeBarrier<PIPE_V>`、`DoSum`、`mean` | `references/guides/sync_cache_coherency_and_norm_tail.md` |
| `exit_code=0` 产出越界索引（`indices_max >= capacity`） | 传入 `seqused_k` / `actual_seq_lengths_key` 超过物理块池 `kvCache.dim0 * blockSize`（尤其含压缩比 `cmpRatio > 1` 时） | `seqused_k`、`actual_seq_lengths_key`、`blockTable`、`cmpRatio`、`kvBlockNum` | `references/guides/mem_window_overflow_and_index_clamp.md` |
| 异常输入本应报错误码却静默返回 `0`（或反之） | 非法负值/越界参数未被主控拦截，或子校验函数返回码被丢弃未向上透传 | `CHECK_RET`、`ACLNN_ERR_PARAM_INVALID`、`161002`、`GRAPH_FAILED` | `references/guides/host_pointer_and_control_tensor.md`、`references/guides/host_seqlen_and_dim_contract.md` |

## 七、上板前静态预防核查入口（仅提供源码无报错日志时按模块核查）

| 待核查代码层或指令族 | 重点核查内容 | 对应专题指南 |
| --- | --- | --- |
| 编译配置、芯片宏与裸核构建 | `--cce-aicore-arch`、`isToUB` 跨代字段守卫、禁递归与虚构 `SetUBSize`、裸核 `ld.lld` 链接 | `references/guides/arch_compile_and_bare_kernel_link.md` |
| 主控入参、放置与切分状态 | 空指针、序列单调、规格上限、主机指针属性、全局 `static` 状态与 64 位乘法防溢出 | `host_*.md`（4 篇） |
| UB / DataCopy / LoadData / Fixpipe | 并发队列容量、32B 对齐跨度、`LoadData3D` 窗口与补齐可行性、`Fixpipe` 字段值域 | `mem_*.md`（5 篇） |
| 同步配对、缓存刷写与数值状态 | 全分支 `Flag`/`Queue` 成对、宽块双槽流水、`DcciUBImpl` 刷缓存、`SetCtrl`/`SetAtomicNone` 复位 | `sync_*.md`、`numeric_domain_and_nan_inf.md` |
