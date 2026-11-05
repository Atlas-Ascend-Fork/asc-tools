# load3d_chn_size_illegal — LoadData3D channelSize 对齐非法

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | load3d_chn_size_illegal |
| 硬件事件编号 | L49（目标）/ ADDR_MISALIGN（实际） |
| 异常类型 | LoadData3D channelSize 对齐非法 |
| 需求目标 | AICoreError.md L49 ILLEGAL_CHN_SIZE — EventID 0x80C98002 |
| 实际覆盖 | ADDR_MISALIGN — 实机触发（channelSize=17 非 16 对齐） |
| 错误码 | errcode:(0, 0x10000, 0)（ADDR_MISALIGN，实机验证值） |
| 类别 | loaddata |
| 源文件 | error/main.asc:83（`uint16_t l1_3d_size = 17;`） |
| 报错模式 | EZ9999 + errcode (0, 0x10000, 0) ADDR_MISALIGN |
| 内存区域 | L1/A2（LoadData3D fmA1→fmA2 搬运路径） |
| API层级 | TC-High |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（l1_3d_size=17→64）+ return 1→0 |
| behavior_note | l1_3d_size=17（half 类型 C0=16，17 非 16 对齐；area=64 小，L46 不触发）使 channelSize 对齐非法；实际触发 ADDR_MISALIGN 而非设计的 L49 ILLEGAL_CHN_SIZE |

## 触发条件

**违规模式**：完整 Cube 流水线（DataCopy→LoadData3D→Mmad→DataCopyCO1），在 LoadData3D 阶段通过 `l1_3d_size = 17`（half 类型 C0=16，17 非 16 对齐；area=64 小，L46 不触发）使 channelSize 对齐非法。实机触发 ADDR_MISALIGN（errcode `(0,0x10000,0)`），与设计的 L49 ILLEGAL_CHN_SIZE 类别不同但同样由 channelSize 对齐问题引发。

**安全模式**：`l1_3d_size = 64` — 恢复 baseline 合法值（16 对齐且 area×64 < L1）。

## 检测规则

### load3d_chn_size_illegal-RULE-01：LoadData3DParamsV2 channelSize 非 C0 对齐

- 【条件】LoadData3DParamsV2 的 channelSize（l1_3d_size）非 C0（=16）对齐
- 【API】`LoadData3DParamsV2<half> ld3d(padList, H, W, l1_3d_size, k, m, kStartPt, mStartPt, strideW, strideH, ...);`
- 【约束】half 类型 C0=16；l1_3d_size=17 → 17 mod 16 = 1 ≠ 0 → 非 C0 对齐 → ADDR_MISALIGN；area=64 小使 L46 不触发
- 【匹配模式】值域判断

### load3d_chn_size_illegal-RULE-02：l1_3d_size 常量注入（17→64）

- 【条件】l1_3d_size 通过 `uint16_t l1_3d_size = 17` 构造，非 16 对齐
- 【API】`uint16_t l1_3d_size = 17;`
- 【约束】l1_3d_size=17 mod 16 = 1 → channelSize 对齐非法 → ADDR_MISALIGN
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 l1_3d_size 从 17 恢复为 baseline 合法值 64（16 对齐且 area×64 < L1）。

```cpp
// 修复前（error）
uint16_t l1_3d_size = 17;

// 修复后（fixed）
uint16_t l1_3d_size = 64;
```

**防御性编程建议**：

- LoadData3D 前 assert `l1_3d_size % C0 == 0`（C0 对齐）
- 封装 LoadData3D 参数校验 wrapper，拒绝 channelSize 非 C0 对齐

## 变体索引

| 变体 | l1_3d_size | l1_3d_size mod 16 | 触发路径 |
|------|-----------|-------------------|---------|
| 基准（本用例） | 17 | 1 | ADDR_MISALIGN |
| 目标变体 | — | — | L49 ILLEGAL_CHN_SIZE（设计目标） |
| 合法变体 | 64 | 0 | 安全（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：channelSize C0 对齐 | l1_3d_size % 16 == 0 | fixed 版（64） |
| EC2：channelSize 非 C0 对齐 | l1_3d_size % 16 != 0 | error 版（17） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| load3d_illegal_stride (13) | 同族 LoadData3D 配置非法，stride 路径 |
| load3d_km_start_illegal (14) | 同族 LoadData3D 配置非法，mStartPt 路径 |
| load3d_km_step_illegal | 同族 LoadData3D 配置非法，kStartPt 路径 |
| mte_atomic_misalign | 同族地址不对齐，AtomicAdd 路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01）、diff_pattern（RULE-02）
- **命中判定**：RULE-01 核对传入 LoadData3D 的 channelSize 对齐，RULE-02 核对 17→64 常量差异；种子 area=64 使 L46 不触发，实际覆盖 ADDR_MISALIGN，不能直接报告为设计目标 L49 ILLEGAL_CHN_SIZE。
  - 读取 error/main.asc 第 83 行 `uint16_t l1_3d_size = 17` 确认非 C0 对齐值。
  - 读取第 37-39 行 `LoadData3DParamsV2<half> ld3d(padList, H, W, l1_3d_size, ...)` + `LoadData(fmA2, fmA1d, ld3d)` 确认 l1_3d_size 传入 LoadData3D。
  - 计算 l1_3d_size=17 mod 16 = 1 ≠ 0 → channelSize 对齐非法 → ADDR_MISALIGN。
  - error 版 stderr 应含 EZ9999 + errcode (0, 0x10000, 0) ADDR_MISALIGN；fixed 版无 EZ9999。
- **修复映射**：
  - load3d_chn_size_illegal-RULE-01 → 使 channelSize 满足 C0 对齐和 L1 容量约束。
  - load3d_chn_size_illegal-RULE-02 → 将 l1_3d_size 从 17 改为 fixed 第 83 行的 64（64 mod 16 = 0，且 area×64 < L1）。
