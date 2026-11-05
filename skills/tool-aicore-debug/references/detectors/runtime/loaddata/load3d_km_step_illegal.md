# load3d_km_step_illegal — LoadData3D kStartPt 导致 MTE 地址不对齐

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | load3d_km_step_illegal |
| 硬件事件编号 | L48（目标）/ L29 ADDR_MISALIGN（实际触发） |
| 异常类型 | LoadData3D kStartPt 导致 MTE 地址不对齐 |
| 需求目标 | AICoreError.md L48 ILLEGAL_K_M_EXT_STEP — EventID 0x80C98002 |
| 实际覆盖 | AICoreError.md L29/L54 ADDR_MISALIGN（MTE 指令地址不对齐）— kStartPt=136 导致 LoadData3D 内部 K 方向偏移产生非对齐地址 |
| 错误码 | errcode:(0, 0x10000, 0) instruction address misalign(ADDR_MISALIGN) |
| 类别 | loaddata |
| 源文件 | error/main.asc:84（`uint16_t kStartPt = 136;`） |
| 报错模式 | EZ9999=YES + retCode=0x26 + errorStr="instruction address misalign(ADDR_MISALIGN)" + mte error info=0x1000000e5 + runtime result=507015 |
| 内存区域 | L1/A2（LoadData3D fmA1→fmA2 搬运路径） |
| API层级 | TC-High |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（kStartPt=136→0）+ return 1→0 |
| behavior_note | kStartPt=136（16 对齐且 < k=144 使 L47 K_start 检查不触发）导致 LoadData3D 内部 K 方向读取地址不对齐 → MTE ADDR_MISALIGN |

## 触发条件

**违规模式**：完整 Cube 流水线（DataCopy→LoadData3D→Mmad→DataCopyCO1）。`kStartPt=136`（16 对齐且 < k=144 使 L47 K_start 检查不触发）导致 LoadData3D 内部 K 方向读取地址不对齐，触发 MTE ADDR_MISALIGN（L29/L54）。L48 ILLEGAL_K_M_EXT_STEP 在当前硬件/骨架下未被硬件直接检出，但 kStartPt=136 经由地址不对齐路径间接触发了 EZ9999。

**安全模式**：`kStartPt = 0` — 恢复合法起始位置。

## 检测规则

### load3d_km_step_illegal-RULE-01：LoadData3DParamsV2 kStartPt 致内部 K 方向地址不对齐

- 【条件】LoadData3DParamsV2 构造参数 kStartPt 导致 LoadData3D 内部 K 方向读取地址非对齐
- 【API】`LoadData3DParamsV2<half> ld3d(padList, H, W, l1_3d_size, k, m, kStartPt, mStartPt, strideW, strideH, ...);`
- 【约束】kStartPt=136（16 对齐但经 LoadData3D 内部偏移后产生非对齐地址）；k=C1×Kh×Kw×C0=1×3×3×16=144 → kStartPt=136 < 144 使 L47 不触发；经内部 K 方向偏移后地址 mod 对齐粒度 != 0 → ADDR_MISALIGN
- 【匹配模式】值域判断

### load3d_km_step_illegal-RULE-02：kStartPt 常量注入（136→0）

- 【条件】kStartPt 通过 `uint16_t kStartPt = 136` 构造，触发地址不对齐
- 【API】`uint16_t kStartPt = 136;`
- 【约束】kStartPt=136 → LoadData3D 内部 K 方向偏移产生非对齐地址 → MTE ADDR_MISALIGN
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 kStartPt 从 136 恢复为合法起始位置 0。

```cpp
// 修复前（error）
uint16_t kStartPt = 136;

// 修复后（fixed）
uint16_t kStartPt = 0;
```

**防御性编程建议**：

- LoadData3D 前 assert kStartPt 为合法对齐值（需结合 k 维度和 LoadData3D 内部偏移规则）
- 避免 kStartPt 取接近 k 上界的非零值（易触发内部地址不对齐）

## 变体索引

| 变体 | kStartPt | k | 实际触发 |
|------|----------|---|---------|
| 基准（本用例） | 136 | 144 | L29/L54 ADDR_MISALIGN |
| 目标变体 | — | — | L48 ILLEGAL_K_M_EXT_STEP（硬件未直接检出） |
| 合法变体 | 0 | 144 | 安全（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：kStartPt 合法 | kStartPt=0（不产生内部地址不对齐） | fixed 版（0） |
| EC2：kStartPt 致 ADDR_MISALIGN | kStartPt≠0 且经内部偏移后地址不对齐 | error 版（136） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| load3d_km_start_illegal (14) | 同族 LoadData3D 配置非法，mStartPt 路径 |
| load3d_illegal_stride (13) | 同族 LoadData3D 配置非法，stride 路径 |
| load3d_chn_size_illegal | 同族 LoadData3D 配置非法，channelSize 路径 |
| mte_atomic_misalign | 同族地址不对齐，AtomicAdd 路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01）、diff_pattern（RULE-02）
- **命中判定**：RULE-01 核对内部 K 方向读取地址的对齐，RULE-02 核对 136→0 常量差异；当前骨架实际覆盖 ADDR_MISALIGN，L48 ILLEGAL_K_M_EXT_STEP 未被硬件直接检出。
  - 读取 error/main.asc 第 84 行 `uint16_t kStartPt = 136` 确认非零起始值。
  - 读取第 37-39 行 `LoadData3DParamsV2<half> ld3d(...)` + `LoadData(fmA2, fmA1d, ld3d)` 确认 kStartPt 传入 LoadData3D。
  - 计算 kStartPt=136（< k=144 使 L47 不触发，但经 LoadData3D 内部 K 方向偏移后地址不对齐）→ MTE ADDR_MISALIGN。
  - error 版 stderr 应含 EZ9999 + retCode=0x26 + errorStr="instruction address misalign(ADDR_MISALIGN)" + mte error info=0x1000000e5 + errcode (0, 0x10000, 0) + runtime result=507015。
- **修复映射**：
  - load3d_km_step_illegal-RULE-01 → 结合 k 维度和 LoadData3D 内部偏移规则，避免产生非对齐读取地址。
  - load3d_km_step_illegal-RULE-02 → 将 kStartPt 从 136 改为 fixed 第 84 行的 `kStartPt = 0`，恢复种子合法起始位置。
