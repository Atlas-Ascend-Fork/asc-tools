# FA 族设计：Mix Kernel（数据流 / 不变量 / 流水 / 跨核同步 / softmax 陷阱）

> **来源（蒸馏）**：Ascend C Tiling 设计方法论·FlashAttention 分册
> foundation/fundamentals.md + design/{shape,resources,execution}.md。
> 覆盖设计节点 N1（宏观形态）/ N2（并行策略）/ N4（softmax）/ N10（流水同步）。

## 1 数学模型与数据流四阶段（FA-2 Online Softmax）

对每个 KV 分块 j 顺序累积：

```
S_j   = (Q · K_jᵀ) × scale                                    # [Bq, Bk]
m_j   = max(m_{j-1}, rowmax(S_j))                              # [Bq]（m_{-1} = -∞）
sum_j = exp(m_{j-1} - m_j) · sum_{j-1} + rowsum(exp(S_j - m_j))# [Bq]
O_j   = exp(m_{j-1} - m_j) · O_{j-1} + exp(S_j - m_j) · V_j    # [Bq, D]
最终:  O = O_last / sum_last
```

**关键性质**：S_j 仅 `[Bq, Bk]` 大小（与 Sk 无关，不实例化全矩阵由此实现）；
跨块累积需 rescale 旧状态 `exp(m_{j-1} - m_j)`；归一化只在末块做一次。

### 四阶段语义（AIC/AIV 协同）

| 阶段 | 核 | 操作 | 同步语义 |
|------|---|------|---------|
| **C1** | AIC | Q·Kᵀ GEMM → `[Bq,Bk]` 中间矩阵到 GM | 完成后通知 AIV |
| **V1** | AIV | scale + softmax + cast → 未归一化 P（`exp(S-m)`）到 GM | 等 C1 / 完成后通知 AIC |
| **C2** | AIC | P·V GEMM → `[Bq,D]` 中间矩阵到 GM | 等 V1 / 完成后通知 AIV |
| **V2** | AIV | 跨块在线累积（rescale + Add）；末块归一化写回 | 等 C2 |

## 2 不变量 I1-I5（违反必崩）

| # | 不变量 | 违反后果 |
|---|--------|---------|
| **I1** 数据流方向 | 同一 KV 分块内 stage 依赖严格 **C1→V1→C2→V2**（不同分块如何交错由设计定） | Fixpipe ADDR_MISALIGN / 数据未就绪 → 全零或乱值 |
| **I2** GQA 同任务 | 同 kvHead 的 G 个 qHead 必须同 task 合并（mEff = curBq × curG 进 Mmad m 维） | KV 重复搬运 G 次，MTE2/L1 利用率塌陷 |
| **I3** 跨 task 状态隔离 | 跨 task 状态（max/sum/O_acc/partial 段）必须按任务分槽隔离 | 被上一任务尾状态污染 → 乱值 |
| **I4** s2 状态累积正确性 | 默认 task 级（s2 不进 taskIdx）或 Split-KV reduce（必补 combine）二选一，禁混用 | 乱值 / 多核死锁 / 精度崩塌 |
| **I5** MX scale 轴对齐 | 量化 scale 沿被消费 matmul 的 K-mmad 轴（V 沿 S_k 不是 D） | Mmad ScaleB 轴错位 → NaN/inf |

## 3 宏观形态（N1）

### Kernel 类型
- 推荐 `__mix__(1, 2)`（1 AIC + 2 AIV）；AIV 端瓶颈时提高 AIV 配比。
- **禁止** `__vector__` 做 FA（性能低 10-100×）。

### UB 模式（streaming 判定）
判定：UB 上是否存在**常驻 ∝ m × D** 的 buffer（如 O_acc 常驻 UB）？
- 有 → 必须改 **streaming UB 模式**：UB 用固定大小（与 D 无关）ping-pong
  buffer；O_acc 移 GM workspace，跨 chunk 读回+rescale+累加+写出；单一代码
  路径覆盖全 D 范围。
- **禁止** `if D ≤ X: 朴素 else: streaming` 两套代码——streaming 在小 D 退化
  为单 chunk loop（典型开销 ≤5%），维护两套成本远高。

### Cube 分块层次
- **L0 K 轴分块**（K_BASE 步长，`Mmad(.k=K_BASE, .cmatrixInitVal=(k==0))`
  自累加）：D > L0 单 fragment 容量时（几乎总触发）；
- **L1 M 轴分块**（M_BASE 行）：`mPad × D × sizeof(Q_T)` 超 L1 单槽时；
- **L1 N 轴分块**（P·V 阶段 V LoadData）：大 D 触发，A5 尤其关键；
  **统一走 N 轴分块代码路径**（小 D 循环 1 次），不按 D 分支。
- V 转置加载用 `LoadDataWithTranspose`（**950PR 仅支持 L1→L0B 通路**）；
  优先支持目标 dtype（bf16）直接转置的形态。
- **Nz2Nz 陷阱**：非转置 load2dV2 不重排分形——上游 Nd2Nz 的 dst 必须让多
  M-fractal 按分形步长**连续排布**才能 `mStep≥2` 批量装载，否则只能单分形
  （牺牲 Cube 批量装载效率）。

## 4 并行策略（N2）

| 策略 | 任务维度 | 适用 |
|------|---------|------|
| **默认 task 级** | `taskIdx → (batch, kvHead, gS1Block, gBlock)`，s2 在 task 内顺序 | 默认；totalTasks ≥ usedCoreNum |
| **Split-KV reduce**（FlashDecoding） | 同一 KV 序列沿 s2 拆多核，各算 partial 后 cross-core combine | totalTasks ≪ usedCoreNum 且 Sq==1（纯 decode）且 Sk > 2K |

Split-KV reduce 启用契约：新增 partial reduce workspace 段（partial O +
partial max/sum）+ 跨核 combine 阶段；softmax 变体走每切片独立计算。
**禁止** totalTasks ≥ usedCoreNum 时启用。

## 5 流水编排与跨核同步（N10）

### 流水级数与 stage 顺序

| 方案 | loop body | 说明 |
|------|-----------|------|
| 2 级（常见首选） | `C1+V1(本轮)` → `C2+V2(loop-PRELOAD_N 轮)` | V1 可先于 V2（ring buffer slot 物理隔离 state） |
| 3 级 | `C1(本轮) → V2(上两轮) → V1+C2(上一轮)` | **V2 必须先于 V1**（V2 读上上轮 state，V1 会覆写） |

### Ring buffer 深度（PRELOAD_N）
默认 PRELOAD_N=2（3 slot）起步，跑通后按 PipeUtilization 在 1/2/3 间微调——
但「是否启用 ring buffer 预取」是**结构决策必须设计期定死**（禁止退化逐块串行）。

### Slot 语义分类（公式分离，禁止共用常量）

| slot 语义 | 取模计数器 | 取模常量 | 索引 | 典型应用 |
|----------|-----------|---------|------|---------|
| 跨 stage 握手 | loop（全局迭代器） | DB（如 2） | `loop & (DB-1)` | 中间矩阵 DB |
| 跨 task 状态 | **mloop（行计数器）** | PRELOAD_N+1（如 3） | `mloop % (PRELOAD_N+1)` | softmax max/sum |
| 跨 loop 自读自写 | loop | PRELOAD_N+1 | `loop % (PRELOAD_N+1)` | V2 的 O_acc 落地段 |
| task 环形缓冲 | loop | CACHE_SIZE（2 的幂） | `loop & (CACHE_SIZE-1)` | task 元信息 |

> ⚠️ `mloop`（行级，softmax max/sum 是行状态）与 `loop`（迭代级，exp buffer）
> **不可混用**——混用症状是 multi-s2 块精度漂移。`PRELOAD_N+1=3` 非 2 的幂
> **必须用 %**；CACHE_SIZE 必须 2 的幂用 `&`（热路径省标量除法）。
> **误用最小复现条件**：single task + multi s2 块（s2N ≥ 3）。

### 跨核同步原语

- 默认 `CrossCoreSetFlag<modeId,pipe>` / `CrossCoreWaitFlag<modeId,pipe>`
  （公开 ISASI，精确通知 fire-and-forget）；
- **两端 modeId 必须一致**：不一致 → flagId 空间独立 → **挂死**。950PR/950DT
  （A5）核内 AIC↔单 AIV 握手用 modeId=4；A2/A3 仅支持 0/1/2。省略模板参数
  退回默认 modeId=0（多核同步空间）→ WaitFlag 永远等不到 → 挂死；
- `PipeBarrier<PIPE_ALL>` 仅契约位置兜底（跨 pipe 无对应 HardEvent 时）——
  **禁止四处散布**（冗余率 <30%，50% 视为性能阻塞项）；
- 同步粒度：per-stage（每 stage 完成即通知）是 FA 4-stage 常见选择；
- 双 AIV（推荐）：双 AIV 全程参与；单 AIV 是简化替代（AIV1 early-return）。

### Fixpipe 陷阱

中间段落地位置（L0C→UB 片上 vs L0C→GM）默认**优选片上**。调用 L0C→UB 必须
**显式构造 `isToUB=true` 的 FixpipeConfig**——默认 `CFG_ROW_MAJOR`（isToUB=false）
指向 GM 物理地址，用它写 UB 目的地**静默出错**（编译过、结果错），极易被
误诊为「硬件不支持 L0C→UB」而错误退到全程 GM。

## 6 V1/V2 Softmax 实现陷阱（N4）

- **实现路径**：默认 `SoftmaxFlashV2` 公开 API（`T` 仅 half/float，bf16 走
  `<float>` + Cast）；`isBasicBlock` 模板参数**必须与 TilingFunc 传值一致**
  ——不一致 → expMax UB 布局错位 → 逐行 rescale 读错偏移 → **编译过静默数值错**。
- **首块初始化契约**（漏则整行 O 错并沿 s2 传播）：max=-∞、sum=0；
  O_acc 的 GM workspace 段**必须显式清零**（GM 默认不清零）；首块无前序状态
  → rescale 因子等价 1（V2 走「跳过 rescale」分支）。
- **末块除零保护**：`O = O_acc / sum` 前对 sum 加 eps/clamp——padding 行或
  极端输入下 sum 可能为 0 → inf/NaN 写出。
- **mask 值用大有限负数（如 -1e30），禁用 -∞**：整行被屏蔽时 `rowmax=-∞` →
  `exp(-∞-(-∞)) = NaN` 沿 s2 累积传播（FA 经典 NaN 陷阱）。
- **完全屏蔽 tile：跳 C1 必须同步跳 V1**——只跳 C1 不跳 V1 → 全 mask 行仍进
  softmax → NaN；跨核握手段需有效数据时 P 写全 0。
- **online softmax 有状态**：跨 s2 块必须接入并回读 running max/sum/rescale
  因子——不能当一次性 `src→dst` 无状态调用。

## 7 编译期特化（N8）

三维度分层判定：

| 判定 | 处理 | FA 典型 |
|------|------|--------|
| 引入/删除 TBuf 成员或改 slot 数 | **编译宏分离 target** | dtype / causal on-off / sparse |
| 热路径分支 | **constexpr 模板参数** | layout（BSND/BNSD）/ hasRope |
| 仅数值参数 | **运行时 if** | 实际序列长度 |

- mix kernel 推荐 **Dummy Block 模式**（Cube 核 VecBlockDummy / 反之）：
  零死代码 + 类型安全；
- causal mask buffer **默认不预分配完整 `[mEff,Bk]` 矩阵**——优先在 S 矩阵
  原地生成（行号公式 + Compare/Select）或单行模板。

## 8 内存划分要点（N7）

- 所有 buffer 公式只允许：运行时查询的硬件常量 + Tiling 基本块 + dtype 字节宽
  ——**禁止直接写数字**；
- 每个 DataCopy 单次量 ≤ 对应 TQue 单槽容量（否则对称 chunk loop，V1/V2
  **两侧都要切**）；
- `Σ(单槽 × depth) ≤ GetCoreMemSize(UB)` 总量校验；
- workspace 多段（握手段 / softmax 状态段 / O_acc 段 / partial 段）独立管理，
  GM 自读自写段禁止裸 DataCopy 不经同步。

## 9 与本仓调优链的衔接

- **基线验证先行**（写自定义 kernel 前必做）：编译运行同类型 mix 示例
  （asc-devkit `matmul_leakyrelu_advanced_api` / `matmul_gelu_high_performance`
  / `softmaxflashv2` building block）确认 `__mix__(1,2)` + TPipe + SoftmaxFlashV2
  在当前 CANN 可用；
- verdict 命中 → 区制 → 杠杆的路由表见 [guide.md](guide.md)；
- overlap_pct（trace_bubble）验证 AIC/AIV 流水化是否生效（pipeline 机制卡
  阈值同款：>30% 生效）。
