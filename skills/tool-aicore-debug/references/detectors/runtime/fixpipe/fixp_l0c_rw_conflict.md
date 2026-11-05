# fixp_l0c_rw_conflict — FIXP L0C 读写冲突（无同步路径尝试，OOB 兜底 L78）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixp_l0c_rw_conflict |
| 硬件事件编号 | L74（尝试）/ L78（OOB 兜底） |
| 异常类型 | FIXP L0C 读写冲突（尝试）/ L0C 读越界（OOB 兜底） |
| 错误码 | errcode:TBD（预期 L78 族 (0, 0x4000000000, 0)） |
| 类别 | fixpipe |
| 源文件 | error/main.asc:77（移除 qCO1.EnQue/DeQue 同步）+ :81（`fp.srcStride = OOB_SRC_STRIDE`） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] |
| 内存区域 | L0C/CO1（CUBE 写 CO1 与 FIXP 读 CO1 地址重叠） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 结构变更（恢复 EnQue/DeQue 同步 + srcStride 3200→m + Fixpipe(c1)→Fixpipe(c1d)）+ return 1→0 |
| behavior_note | 移除 Mmad→Fixpipe 间 TQue 同步尝试 L74 冲突；srcStride=OOB_SRC_STRIDE(3200) 放大致 L0C 读越界 L78 OOB 兜底；TC-High 编译器隐式同步使 L74 大概率不可达 |

## 触发条件

**违规模式**：`__cube__` TC-High kernel，完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe）。**主目标 L74**：移除 `qCO1.EnQue/DeQue` 在 Mmad（CUBE 写 CO1）与 Fixpipe（FIXP 读 CO1）间的 flag 同步，使 CUBE 写与 FIXP 读在同一 CO1 地址时间重叠，尝试制造 L0C 读写冲突。**OOB 兜底**：`fp.srcStride = OOB_SRC_STRIDE(3200)`（=m×100=32×100），单块读取 stride 远超 CO1 1024-float 缓冲 → L0C 读越界 L78 → EZ9999。

**安全模式**：恢复 `qCO1.EnQue<float>(c1)` + `qCO1.DeQue<float>()` 同步，`fp.srcStride = m`（合法 32），`Fixpipe(cGM, c1d, fp)`（经 DeQue 的 c1d）。

## 检测规则

### fixp_l0c_rw_conflict-RULE-01：移除 Mmad→Fixpipe 间 TQue 同步（主路径 L74）

- 【条件】Mmad 写 CO1 后移除 qCO1.EnQue/DeQue，直接 Fixpipe 读同一 c1 tensor，CUBE 写与 FIXP 读无 flag 同步
- 【API】error：`Mmad(c1, ...); ... Fixpipe(cGM, c1, fp);`（无 EnQue/DeQue）vs fixed：`Mmad(c1, ...); qCO1.EnQue<float>(c1); c1d = qCO1.DeQue<float>(); Fixpipe(cGM, c1d, fp);`
- 【约束】TQue 同步（EnQue/DeQue）保证 CUBE pipe 完成后 FIXP pipe 才读；移除后读写时间重叠 → L74（TC-High 编译器隐式同步使大概率不可达）
- 【匹配模式】diff_pattern

### fixp_l0c_rw_conflict-RULE-02：srcStride OOB 兜底（L78 实际覆盖）

- 【条件】`fp.srcStride = OOB_SRC_STRIDE`（=3200）远超 CO1 buffer 容量，单块 L0C 读越界
- 【API】`fp.srcStride = OOB_SRC_STRIDE;`（error 第 81 行）vs `fp.srcStride = m;`（fixed）
- 【约束】OOB_SRC_STRIDE=3200 > CO1 容量 1024 floats → L0C 读越界 = L78
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：恢复 Mmad→Fixpipe 间 TQue 同步（EnQue/DeQue），srcStride 恢复合法值 m。

```cpp
// 修复前（error）
Mmad(c1, a2d, b2d, {m, n, k, 0, false, true});
qA2.FreeTensor(a2d); qB2.FreeTensor(b2d);
// 无 EnQue/DeQue，直接使用 c1
FixpipeParamsV220 fp;
fp.srcStride = OOB_SRC_STRIDE;
fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;
Fixpipe(cGM, c1, fp);
qCO1.FreeTensor(c1);

// 修复后（fixed）
Mmad(c1, a2d, b2d, {m, n, k, 0, false, true});
qCO1.EnQue<float>(c1);
LocalTensor<float> c1d = qCO1.DeQue<float>();
qA2.FreeTensor(a2d); qB2.FreeTensor(b2d);
FixpipeParamsV220 fp;
fp.srcStride = m;
fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;
Fixpipe(cGM, c1d, fp);
qCO1.FreeTensor(c1d);
```

**防御性编程建议**：

- Mmad 写 CO1 后必须 EnQue/DeQue 同步，禁止跨管线直接读未同步 tensor
- TC-High 编译器基于 LocalTensor 依赖追踪，隐式同步可能使 L74 不可达

## 变体索引

| 变体 | 同步状态 | srcStride | 主目标 | 实际覆盖 |
|------|---------|-----------|--------|---------|
| 基准（本用例） | 移除 EnQue/DeQue | OOB_SRC_STRIDE(3200) | L74 冲突 | L78 OOB 兜底 |
| 安全变体 | 保留 EnQue/DeQue | m(32) | — | 无异常（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：TQue 同步正常 | EnQue/DeQue 保留，srcStride 合法 | fixed 版 |
| EC2：无同步 + OOB 兜底 | 移除 EnQue/DeQue + srcStride OOB → L78 兜底 | error 版 |
| EC3：L74 冲突独立命中 | 签名含独立冲突特征（cube error info 读写冲突标志位） | 未发生（大概率不可达） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| fixpipe_addr_misalign (15) | 同族 srcStride OOB 路径（L78），主要重复风险源 |
| fixpipe_l0c_conflict (16) | 同族 L74 尝试，dstStride=0 路径（L77 实际覆盖） |
| fixpipe_biu_error (L85) | 同族 Fixpipe 路径，GM>48bit BIU 响应路径 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（`fixp_l0c_rw_conflict-RULE-01`）；diff_pattern（`fixp_l0c_rw_conflict-RULE-02`）。
- **命中判定**：按 RULE-01 对照 Mmad→Fixpipe 间 EnQue/DeQue 及 tensor 使用差异，按 RULE-02 核对 srcStride OOB；保留 TC-High 编译器隐式同步可能使 L74 不可达的限制，缺失显式同步不能单独确认 L74，须结合下述独立冲突签名。保留以下定位、修复对照和运行签名核对步骤：

  1. 读取 error/main.asc 第 77 行，确认 Mmad 后无 `qCO1.EnQue`（移除同步）。
  2. 确认第 81 行 `fp.srcStride = OOB_SRC_STRIDE`（第 10 行 OOB_SRC_STRIDE=3200）。
  3. 计算 srcStride=3200 > CO1 容量 1024 floats → L0C 读越界（OOB 兜底 L78）。
  4. fixed 版恢复 EnQue/DeQue（第 77/80 行）+ srcStride=m + Fixpipe(c1d) → 安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x26。
  6. 签名判别：若与 fixpipe_addr_misalign 整体一致 → 标 DUPLICATE_COVERAGE；若含 L74 独立冲突特征 → 判命中 L74。
- **修复映射**：
  - `fixp_l0c_rw_conflict-RULE-01` → 恢复 `qCO1.EnQue<float>(c1)` 和 `qCO1.DeQue<float>()`，让 Fixpipe 使用出队后的 c1d，并按原修复方案释放 c1d。
  - `fixp_l0c_rw_conflict-RULE-02` → 将 `fp.srcStride = OOB_SRC_STRIDE` 恢复为 `fp.srcStride = m`，消除 L78 读越界兜底。
