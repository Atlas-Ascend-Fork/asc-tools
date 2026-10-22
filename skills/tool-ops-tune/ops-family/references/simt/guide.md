# SIMT 族指南（guide）——线程级算子编程范式与调优

> **来源（蒸馏）**：Ascend C SIMT 最佳实践（13 篇参考文档的
> 范式提炼）——此前本族 guide 仅 4 行骨架。SIMT 基础概念（核函数/线程架构/
> 内存层级）查 `ascendc-docs-search`，本文聚焦编程范式与调优要点。

## 族定位

条件分支、离散索引、gather-scatter、MoE routing 等不规则操作——SIMD 向量
路径的分支/离散访存代价过高时选 SIMT（DAV_3510，每 AIV 4 个 warp scheduler）。

**SIMT vs SIMD 选型速查**：

| 维度 | SIMT | SIMD |
|------|------|------|
| 编程模型 | 多线程并行，每线程独立执行 | 单核向量指令，整块数据并行 |
| 分支/离散索引 | 原生支持 | 代数化替代（Compare/Select/Gather）代价高 |
| 数据布局 | 逐元素寻址自由 | 连续对齐高效 |
| IPC 特征 | Simt_*IPC 可观测 | SIMD_ExecIPC（双发上限 2.0） |

## 核心编程范式（三件套）

### 1. VF 声明 + 调用

```cpp
// 声明（__simt_vf__ 标记 + LAUNCH_BOUND 寄存器预算）
template <typename T>
__simt_vf__ __aicore__ LAUNCH_BOUND(512) inline void OpSimt(
    uint64_t count, __gm__ T* x, __gm__ T* y);

// 调用（Dim3 线程数必须是编译期常量；GM_ADDR 直接转换禁 GlobalTensor 中转）
AscendC::Simt::VF_CALL<OpSimt<T>>(AscendC::Simt::Dim3(512), count, (__gm__ T*)x, (__gm__ T*)y);
```

### 2. 线程排布统一 for 循环

```cpp
for (uint64_t i = AscendC::Simt::GetBlockIdx() * AscendC::Simt::GetThreadNum()
                  + AscendC::Simt::GetThreadIdx();
     i < count;
     i += AscendC::Simt::GetThreadNum() * AscendC::Simt::GetBlockNum()) {
    // ...
}
```

初始 = 线程全局线性化位置；步进 = 全核全线程总数（每元素恰被一个线程处理）。

### 3. GM 直通 vs UB 中转

- 简单场景：VF 内直接 `__gm__` 指针读写（省一趟 UB）；
- 跨线程共享数据：UB 中转（见下）。

## 核内共享内存（类 CUDA shared memory）

```cpp
TBuf<QuePosition::VECCALC> sharedBuf_;
pipe_->InitBuffer(sharedBuf_, 2048);                       // kernel 侧申请
__ubuf__ uint32_t* p = (__ubuf__ uint32_t*)sharedBuf_.Get<uint32_t>().GetPhyAddr();
Simt::VF_CALL<OpSimt<T>>(Simt::Dim3(N), ..., p);           // 传入 VF
```

- 同地址并发写必须原子操作（`asc_atomic_add` 等）；
- buffer 大小 tiling 侧 `SetLocalMemorySize` 预留。

## 多核同步

- 跨核数据交换用 workspace（tiling `SetWorkspaceSize`）+ kernel 侧 `SyncAll()`
  + tiling `context->SetScheduleMode(1)`（同步模式）——三者缺一不可；
- **同 kernel 多个 VF_CALL 之间只保证 block 内串行，不保证全核同步**——
  需要全核同步的位置显式 `SyncAll()`。

## 判定规则映射（verdict 命中 → 本族调优）

| verdict 命中 | SIMT 场景解读 | 处置 |
|-------------|--------------|------|
| L1_SCALAR_BOUND | 线程内分支/地址计算过重 | 循环不变量外提；索引计算 API 化 |
| L1_VEC_BOUND（AIVx_SIMT 主导） | warp 占用不足 | 增大 Dim3 线程数；LAUNCH_BOUND 重估 |
| L0_STALL_DOMINANT | warp stall（需 PcSampling/trace_bubble） | 按 stall 类别处置（加载/分支/同步） |
| 串行指纹 | VF_CALL 间无重叠 | 多 VF 拆分改单 VF 内循环 |

## 深度参考（参考实现，13 篇）

vf-declaration / vf-call / thread-stride-pattern / index-calculation /
data-transfer / compute-operators / multi-core-sync / intra-core-shared /
tensorlist-handling / 宏 API / 调测接口 / 编译与运行 / overview——API 分类
索引与完整代码示例在该 skill（本 guide 为其范式浓缩 + 本仓 verdict 映射）。

## 模板状态（如实标注）

本族无可拷贝模板（模板数 0）——按本文三件套范式 + 上表调优路由实施；
SIMT API 用法以 asc-devkit `SIMT-API/` 文档为准。
