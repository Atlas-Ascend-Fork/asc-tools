# load3d_km_start_illegal — LoadData3D mStartPt 非法（M 方向起始位置超界）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | load3d_km_start_illegal |
| 硬件事件编号 | 14 |
| 异常类型 | LoadData3D mStartPt 非法（配置非法类异常） |
| 错误码 | errcode:(0, 0x8000000, 0) The value of K_M_START_POS is illegal |
| 类别 | loaddata |
| 源文件 | error/main.asc:85（`uint16_t mStartPt = 200;`） |
| 报错模式 | EZ9999（验证阶段补充 retCode/exit_code 字段） |
| 内存区域 | L1/A2（LoadData3D fmA1→fmA2 搬运路径） |
| API层级 | TC-High |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（mStartPt=200→0）+ return 1→0 |
| behavior_note | LoadData3D mStartPt=200 使 M 方向起始位置超出合法范围，触发 AICoreError.md L47 ILLEGAL_K_M_START_POS；error 版与 Seed-W10 逐字等价（已验证可触发） |

## 触发条件

**违规模式**：完整 Cube 流水线（DataCopy→LoadData3D→Mmad→DataCopyCO1），`LoadData3DParamsV2<half>` 构造时 `mStartPt = 200` 使 M 方向起始位置超出合法范围，触发 AICoreError.md L47 ILLEGAL_K_M_START_POS 检测。

**安全模式**：`mStartPt = 0` — 恢复合法起始位置。

## 检测规则

### load3d_km_start_illegal-RULE-01：LoadData3DParamsV2 mStartPt 超合法范围

- 【条件】LoadData3DParamsV2 构造参数 mStartPt 超出 M 方向合法起始位置范围
- 【API】`LoadData3DParamsV2<half> ld3d(padList, H, W, l1_3d_size, k, m, kStartPt, mStartPt, strideW, strideH, ...);`
- 【约束】mStartPt 合法范围由 M 维度限定；mStartPt=200 >> m=16 → AICoreError.md L47 ILLEGAL_K_M_START_POS；errcode (0, 0x8000000, 0)
- 【匹配模式】值域判断

### load3d_km_start_illegal-RULE-02：mStartPt 常量注入（200→0）

- 【条件】mStartPt 通过 `uint16_t mStartPt = 200` 构造，超出合法起始位置
- 【API】`uint16_t mStartPt = 200;`
- 【约束】mStartPt=200 远超 m=16 的合法范围 → ILLEGAL_K_M_START_POS
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 mStartPt 从 200 恢复为合法起始位置 0。

```cpp
// 修复前（error）
uint16_t mStartPt = 200;

// 修复后（fixed）
uint16_t mStartPt = 0;
```

**防御性编程建议**：

- LoadData3D 前 assert `mStartPt < m`（M 方向合法范围）
- 封装 LoadData3D 参数校验 wrapper，拒绝 mStartPt 超界

## 变体索引

| 变体 | mStartPt | m | 触发路径 |
|------|----------|---|---------|
| 基准（本用例） | 200 | 16 | L47 ILLEGAL_K_M_START_POS |
| 边界变体 | 16 | 16 | L47（恰好越界） |
| 合法变体 | 0 | 16 | 安全（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：mStartPt 合法 | mStartPt < m（M 方向合法范围） | fixed 版（0 < 16） |
| EC2：mStartPt 非法 | mStartPt >= m | error 版（200 >= 16） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| load3d_illegal_stride (13) | 同族 LoadData3D 配置非法，stride 路径 |
| load3d_km_step_illegal | 同族 LoadData3D 配置非法，kStartPt 路径 |
| load3d_chn_size_illegal | 同族 LoadData3D 配置非法，channelSize 路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01）、diff_pattern（RULE-02）
- **命中判定**：RULE-01 在 mStartPt 超出 M 方向合法范围时命中，RULE-02 核对 200→0 常量差异；种子 m=16，边界 mStartPt=16 也属于非法范围。
  - 读取 error/main.asc 第 85 行 `uint16_t mStartPt = 200` 确认超界值。
  - 读取第 37-39 行 `LoadData3DParamsV2<half> ld3d(...)` + `LoadData(fmA2, fmA1d, ld3d)` 确认 mStartPt 传入 LoadData3D。
  - 计算 mStartPt=200 > m=16 → 触发 AICoreError.md L47 ILLEGAL_K_M_START_POS；errcode (0, 0x8000000, 0)。
  - error 版 stderr 应含 EZ9999（验证阶段补充 retCode/errcode）。
- **修复映射**：
  - load3d_km_start_illegal-RULE-01 → 将 mStartPt 限制在 M 方向合法范围 `mStartPt < m`。
  - load3d_km_start_illegal-RULE-02 → 将 mStartPt 从 200 改为 fixed 第 85 行的 `mStartPt = 0`。
