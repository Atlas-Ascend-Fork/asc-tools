# vec_gm_addr_oob — VEC Dcache 读 GM 地址越 48bit

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | vec_gm_addr_oob |
| 硬件事件编号 | L159 |
| 异常类型 | VEC Dcache 读 GM 地址越 48bit |
| 错误码 | errcode:TBD（GM>48bit 检测路径无先例） |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:16（`xGlobal.SetGlobalBuffer((__gm__ float*)((uintptr_t)x + (1ULL << 48)), 256)`） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| 内存区域 | GM（`__gm__` float*，偏移 1ULL<<48 致 GlobalTensor 基址 bit[48]≠0） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | GlobalTensor 基址 2^48 偏移注入（`(uintptr_t)x+(1ULL<<48)` → `x`）+ return 1→0 |
| behavior_note | Init 阶段 SetGlobalBuffer 接受偏移指针致 GlobalTensor 基址 bit[48]≠0，Process 阶段 DataCopy 时 VEC/Dcache 读 GM 超 48bit → L159 |

## 触发条件

**违规模式**：TC-High kernel class（VEC 属性），Init 阶段 `xGlobal.SetGlobalBuffer` 接受偏移指针 `(__gm__ float*)((uintptr_t)x + (1ULL << 48))` 使 GlobalTensor 基址 bit[48]≠0（超 48bit GM 物理地址空间），Process 阶段 `DataCopy(xL, xGlobal, 256)` 时 VEC/Dcache 读 GM 地址超 48bit → L159。

**安全模式**：`xGlobal.SetGlobalBuffer(x, 256);`（合法 GM 基址）。

## 检测规则

### vec_gm_addr_oob-RULE-01：GlobalTensor 基址 2^48 偏移注入

- 【条件】`SetGlobalBuffer` 的 `__gm__` 指针通过 `(uintptr_t)base + (1ULL << 48)` 构造，使地址 bit[48]≠0
- 【API】`xGlobal.SetGlobalBuffer((__gm__ float*)((uintptr_t)x + (1ULL << 48)), 256);`（error 第 16 行）vs `xGlobal.SetGlobalBuffer(x, 256);`（fixed 第 16 行）
- 【约束】GM 物理地址空间 48bit，偏移 2^48 致 GlobalTensor 基址 bit[48]≠0 超 48bit
- 【匹配模式】diff_pattern

### vec_gm_addr_oob-RULE-02：DataCopy 读 GM 地址超 48bit

- 【条件】`DataCopy(xL, xGlobal, 256)` 的源 GlobalTensor 基址 bit[48]≠0
- 【API】`DataCopy(xL, xGlobal, 256);`（Process 阶段）
- 【约束】xGlobal 基址 bit[48]≠0 → VEC/Dcache 读 GM 地址超 48bit → L159
- 【匹配模式】存在性检查

## 修复方案

**标准修复模式**：移除 GlobalTensor 基址 2^48 偏移，使用合法基址。

```cpp
// 修复前（error）
xGlobal.SetGlobalBuffer((__gm__ float*)((uintptr_t)x + (1ULL << 48)), 256);

// 修复后（fixed）
xGlobal.SetGlobalBuffer(x, 256);
```

**防御性编程建议**：

- 禁止对 GM 指针叠加 ≥ 2^48 的偏移常量
- SetGlobalBuffer 前断言地址 < (1ULL << 48)

## 变体索引

| 变体 | 偏移表达式 | 地址 bit[48] | 结果 |
|------|-----------|-------------|------|
| 基准（本用例） | (1ULL << 48) | =1 | L159 GM>48bit（VEC/Dcache） |
| 边界变体 | (1ULL << 47) | =0 | 地址合法（48bit 内） |
| GM-alloc-OOB | alloc 越界 | =0 | 地址越界（不同检测路径） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：GM 地址合法 | 地址 < 2^48 且在分配范围内 | fixed 版 |
| EC2：GM>48bit | 地址 bit[48]≠0（地址格式非法） | error 版 |
| EC3：GM-alloc-OOB | 地址在 48bit 内但越界 | 已覆盖 gm_addr_oob |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| scalar_gm_addr_oob (L107) | 同族 GM>48bit，scalar/MTE 路径 |
| vec_neg_sqrt (1) | 来源路径（Seed-V3 框架），同族 VEC 地址越界 |
| gm_addr_oob | 同族地址越界，GM-alloc-OOB 路径 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01）、存在性检查（RULE-02）。
- **命中判定**：按下列步骤核对 RULE-01 的 GlobalTensor 基址偏移差异；RULE-02 还需确认 Process 阶段 `DataCopy(xL, xGlobal, 256)` 读取该异常基址。
  1. 读取 error/main.asc 第 16 行 `xGlobal.SetGlobalBuffer((__gm__ float*)((uintptr_t)x + (1ULL << 48)), 256)`。
  2. 判定偏移 2^48 致 GlobalTensor 基址 bit[48]≠0 → 超 48bit GM 物理地址空间。
  3. fixed 版第 16 行 `xGlobal.SetGlobalBuffer(x, 256)`（合法基址）→ 安全。
  4. error 版 stderr 应含 EZ9999（验证阶段确认 errcode，L159 VEC Dcache GM>48bit）。
  - `runtime_required=true`：L159 的运行时归属需 errcode 确认；仅有地址构造与 EZ9999 不能替代该确认。
- **修复映射**：RULE-01、RULE-02 → 移除 GlobalTensor 基址的 `1ULL << 48` 偏移，恢复 `xGlobal.SetGlobalBuffer(x, 256)`，后续 DataCopy 使用该合法基址。
