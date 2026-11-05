# mte_instr_illegal_cfg — MTE INSTR_ILLEGAL_CFG（指令配置非法）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | mte_instr_illegal_cfg |
| 硬件事件编号 | L31/L55 |
| 异常类型 | MTE INSTR_ILLEGAL_CFG（指令配置非法） |
| 错误码 | errcode:(TBD, TBD, 0)（L31/L55 主路径无先例，OOB 降级 UB OOB 族） |
| 类别 | alignment_config |
| 源文件 | error/main.asc:30-35（DataCopyPad 非法 padding + OOB 兜底） |
| 报错模式 | EZ9999 + retCode=0x31 [vector core exception] + aic error mask=0x6500020bd00028c |
| 内存区域 | UB（`__ubuf__`，DataCopyPad/DataCopy 目标） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 结构变更（非法 padding+OOB 兜底→合法 padding+正常 DataCopy）+ return 1→0 |
| behavior_note | 主路径 DataCopyPad padding=255 探索 L31/L55；OOB 兜底 DataCopy count=65535 致 UB OOB |

## 触发条件

**违规模式（主路径）**：`DataCopyPad(zL, xGlobal, dataParams, padParams)` with `padParams(true, 255, 255, (half)0)` — leftPadding/rightPadding=255（uint8_t max）下发非法 padding 组合 → MTE 收到不支持的指令配置（L31/L55 探索）。

**违规模式（OOB 兜底）**：`DataCopy(zL, xGlobal, 65535)` — 读 65535 half 入 256 half 的 UB → VEC/MTE UB OOB。

**安全模式**：`padParams(false, 0, 0, (half)0)`（isPad=false，合法 padding 0）；移除 OOB 兜底，改 `DataCopy(zGlobal, zL, TOTAL_LENGTH)`（合法 UB→GM）。

## 检测规则

### mte_instr_illegal_cfg-RULE-01：DataCopyPad padding 值超合法范围

- 【条件】DataCopyPadExtParams 的 leftPadding/rightPadding 值为非法组合（如 255,255）
- 【API】`DataCopyPadExtParams<half> padParams(true, 255, 255, (half)0); DataCopyPad(zL, xGlobal, dataParams, padParams);`
- 【约束】padding=255（uint8_t max）下发不支持的 padding 组合 → MTE 指令配置非法
- 【匹配模式】值域判断

### mte_instr_illegal_cfg-RULE-02：DataCopy count 超 UB 分配大小（OOB 兜底）

- 【条件】DataCopy 的 count 参数超过目标 LocalTensor 的 UB 分配大小
- 【API】`DataCopy(zL, xGlobal, 65535)`；zL 分配 256 half
- 【约束】count=65535 >> 256（UB 分配）→ UB OOB
- 【匹配模式】值域判断

### mte_instr_illegal_cfg-RULE-03：DataCopyExtParams blockSize 与实际不一致

- 【条件】DataCopyExtParams 的 blockSize 与实际数据布局不匹配
- 【API】`DataCopyExtParams dataParams(1, 16, 0, 0, 0)`（error）vs `DataCopyExtParams dataParams(1, TOTAL_LENGTH * sizeof(half), 0, 0, 0)`（fixed）
- 【约束】error 版 blockSize=16 与实际 TOTAL_LENGTH×sizeof(half)=512 不一致
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：padParams 设为合法（isPad=false, padding=0），移除 OOB 兜底行，改正常 UB→GM DataCopy。

```cpp
// 修复前（error）
DataCopyExtParams dataParams(1, 16, 0, 0, 0);
DataCopyPadExtParams<half> padParams(true, 255, 255, (half)0);
DataCopyPad(zL, xGlobal, dataParams, padParams);
DataCopy(zL, xGlobal, 65535);
outQ.EnQue(zL);
inQ.FreeTensor(xL);

// 修复后（fixed）
DataCopyExtParams dataParams(1, TOTAL_LENGTH * sizeof(half), 0, 0, 0);
DataCopyPadExtParams<half> padParams(false, 0, 0, (half)0);
DataCopy(zGlobal, zL, TOTAL_LENGTH);
outQ.FreeTensor(zL);
```

**防御性编程建议**：

- DataCopyPad 前 assert padding 值在硬件支持范围 `[0, max_legal_padding]`
- DataCopy 前 assert `count <= localTensor.GetSize()`
- DataCopyExtParams blockSize 应与 `count * sizeof(T)` 一致

## 变体索引

| 变体 | padding | isPad | DataCopy count | 触发路径 |
|------|---------|-------|---------------|---------|
| 基准-主路径（本用例） | (255,255) | true | 65535 | L31/L55 + UB OOB |
| 仅 padding 变体 | (255,255) | true | TOTAL_LENGTH | L31/L55 |
| 仅 OOB 变体 | (0,0) | false | 65535 | UB OOB |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：DataCopyPad 合法 | padding ≤ 合法阈值 且 isPad 合理 | fixed 版（false,0,0） |
| EC2：DataCopyPad 非法 | padding > 合法阈值 | error 版主路径（255,255） |
| EC3：DataCopy count 超 UB | count > InitBuffer 大小 | error 版兜底（65535 > 256） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| vec_neg_sqrt | 同骨架 TC-High kernel class，本用例为 vec_neg_sqrt 框架变体 |
| vec_ub_forbidden_access | 同族 UB OOB（OOB 兜底路径） |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/RULE-02）、diff_pattern（RULE-03）
- **命中判定**：RULE-01 核对 padding 组合，RULE-02 核对 DataCopy count 与 UB 分配大小；RULE-03 比较 blockSize=16 与实际 `TOTAL_LENGTH * sizeof(half)=512`。主路径 L31/L55 为探索性配置，须与 OOB 兜底路径分别记录。
  - 读取 error/main.asc 第 30 行 `DataCopyExtParams(1, 16, 0, 0, 0)` 确认 blockSize。
  - 读取第 31 行 `DataCopyPadExtParams<half>(true, 255, 255, (half)0)` 确认非法 padding。
  - 读取第 32 行 `DataCopyPad(zL, xGlobal, dataParams, padParams)` 主路径调用。
  - 读取第 35 行 `DataCopy(zL, xGlobal, 65535)` OOB 兜底（count=65535 > UB 256 half）。
  - error 版 stderr 应含 EZ9999 + retCode=0x31 + aic error mask=0x6500020bd00028c。
- **修复映射**：
  - mte_instr_illegal_cfg-RULE-01 → 将 padding 改为 fixed 版的 `(false, 0, 0)` 合法配置。
  - mte_instr_illegal_cfg-RULE-02 → 移除 `DataCopy(zL, xGlobal, 65535)` OOB 兜底，改为 fixed 版的 `DataCopy(zGlobal, zL, TOTAL_LENGTH)`。
  - mte_instr_illegal_cfg-RULE-03 → 将 blockSize 从 `16` 改为 `TOTAL_LENGTH * sizeof(half)`（种子为 512），与实际数据布局一致。
