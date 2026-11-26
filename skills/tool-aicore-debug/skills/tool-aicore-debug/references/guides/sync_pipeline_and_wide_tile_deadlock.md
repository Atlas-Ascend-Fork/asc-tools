# 流水同步配对、L0 读写冲突与宽块双槽（STAGES）跨核死锁修复指南

## 一、触发指纹与子例对比

- **典型报错**：同步返回码 `507014`（`aicore timeout`）、进程超时 `exit_code=124`（空 `stderr` 或 `timeout or trap error`）、`errcode ∈ {14, 39}`（`cube_l0b_rdwr_cflt` / `mte_l0b_rdwr_cflt`）或 `fixp_l0c_rw_conflict`。
- **过/崩子例对比**：
  1. **宽块死锁**：`K=128`（窄分块）通过，`K=256` 或 `V=256`（宽分块）永久卡死超时；
  2. **分支漏配对**：常规整除块通过，走入早退 `if (...) return/continue`、空任务核或尾块分支时永久等待；
  3. **全核栅栏失配**：核数跑满时通过，小批量部分核无任务直接 `return` 时卡死在 `SyncAll()`。
- **30 秒源码搜索词**：在 `op_kernel/` 中搜 `STAGES`、`L1B_STAGES`、`SetFlag`、`WaitFlag`、`CrossCoreSetFlag`、`CrossCoreWaitFlag`、`SyncAll`、`EnQue`、`DeQue`、`AllocTensor`、`FreeTensor`。

## 二、必查参数与公式清单

1. **多阶段融合算子宽块（`K/V > 128`）全链路死锁联查**：
   - **前段矩阵调度（Prepare / Score）**：检查 Catlass / `MmadPingpong` 模板在宽维度下是否误用单槽 `L1B_STAGES = 1`（预取下一块覆盖或互锁当前块），宽维 `K > 128` 时须切换双槽策略 `L1B_STAGES = 2`；
   - **后段跨核阶段（如 `Post-WU` / `FwdH` 等后续流水）**：切忌只修第 1 个子阶段就停手！同一算子后续阶段若分块宽度 `> 128`，会导致片上双缓冲放不下或 `AIV <-> AIC` 跨核握手循环次数不一致而再次死锁，必须同步把后续阶段单次宽块收敛到 `<= 128`（拆成 `2` 趟子块）并核对收发双方循环次数完全相等。
2. **队列与事件全控制流 1:1 配对（防死锁与计数器上溢）**：
   - 同一 `TQue` 在每条可达路径（含 `continue`/`break`/`return`）上必须严格满足 `AllocTensor -> EnQue -> DeQue -> FreeTensor` 次数相等、顺序一致；
   - 每一条 `WaitFlag<Src, Dst>(id)` 必须有先行的同类型同 `id` `SetFlag<Src, Dst>(id)`；且禁止连续发射超过硬件上限（如 `>= 16` 次）无配对 `WaitFlag` 的 `SetFlag` / 跨核 arrive，防止同步计数器上溢。
3. **跨核 `SyncAll()` 启动核数一致性**：凡核函数内调用 `SyncAll()`，主控要么 `context->SetBlockDim(actualUsedCores)` 只启动有任务的核，要么让无任务空闲核跳过访存但照常走过同一个 `SyncAll()`，严禁部分核提前 `return`。
4. **直管矩阵流水 L0A / L0B / L0C 读写同步（防 `errcode=14 / 39`）**：手写 `LoadData` -> `Mmad` -> `Fixpipe` 循环时，回写重用 L0A/L0B 前必须等待 `PIPE_M -> PIPE_MTE1` 完成，读取 L0C 前必须等待 `PIPE_M -> PIPE_FIX` 完成，下一轮累加写 L0C 前必须等待 `PIPE_FIX -> PIPE_M` 完成。

## 三、常见误区（避坑）

- **误区 1**：遇到挂起直接把卡住的 `WaitFlag` 或 `SyncAll` 删掉，虽然不挂死但立刻造成数据竞争和随机静默精度损坏。
- **误区 2**：修宽块挂死只改了 `prepare` 头文件的 `L1B_STAGES=2`，没查同一算子的 `post_wu` / `fwd_h` 宽块切分，导致大子例依然超时。

## 四、标准修复代码模板

```cpp
// 1. 宽块双槽分发 + 后端阶段块宽收敛至 <= 128（保持 AIV 与 AIC 跨核握手次数严格一致）
using DispatchPolicy = std::conditional_t<(K_DIM > 128),
    GemmPingpongPolicy</*L1A_STAGES=*/2, /*L1B_STAGES=*/2>,
    GemmPingpongPolicy</*L1A_STAGES=*/2, /*L1B_STAGES=*/1>>;
constexpr uint32_t SUB_TILE_K = (K_DIM > 128) ? 128 : K_DIM;
constexpr uint32_t K_STEPS = (K_DIM + SUB_TILE_K - 1) / SUB_TILE_K; // AIC 与 AIV 共用同一 K_STEPS 循环握手

// 2. 空闲核不早退，安全走过全核 SyncAll()
if (blockIdx < usedCoreNum) {
    ProcessValidCore(blockIdx);
}
AscendC::SyncAll(); // 所有启动核必须全部到达
```
