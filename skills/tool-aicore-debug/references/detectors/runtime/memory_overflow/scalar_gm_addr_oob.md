# scalar_gm_addr_oob — SU SEQ 指令序列错误（scalar 访问 GM 地址超 48bit）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | scalar_gm_addr_oob |
| 硬件事件编号 | L107 |
| 异常类型 | SU SEQ 指令序列错误 — scalar 访问 GM 地址超 48bit |
| 错误码 | errcode:TBD（GM>48bit 检测路径无先例） |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:15（`__gm__ half* badPtr = (__gm__ half*)((uintptr_t)a + (1ULL << 48))`） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| 内存区域 | GM（`__gm__` half*，偏移 1ULL<<48 致 bit[48]≠0） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | GM 指针 2^48 偏移注入（`(uintptr_t)a+(1ULL<<48)` → `a`）+ return 1→0 |
| behavior_note | badPtr GM 地址 bit[48]≠0 超 48bit 物理地址空间，asc_copy_gm2ub_sync 经 badPtr 读 GM → SU SEQ 地址格式非法 → L107 |

## 触发条件

**违规模式**：TC-Low 平坦 `__vector__` 函数，构造 `badPtr = (__gm__ half*)((uintptr_t)a + (1ULL << 48))` 使 GM 访问地址 bit[48]≠0（超 48bit GM 物理地址空间），经 `asc_copy_gm2ub_sync` 使 scalar/MTE 经 badPtr 读 GM → SU SEQ 指令序列检测地址格式非法 → L107。

**安全模式**：`__gm__ half* badPtr = a;`（合法 GM 基址，无偏移）。

## 检测规则

### scalar_gm_addr_oob-RULE-01：__gm__ 指针 2^48 偏移注入

- 【条件】`__gm__` 指针通过 `(uintptr_t)base + (1ULL << 48)` 构造，使地址 bit[48]≠0
- 【API】`__gm__ half* badPtr = (__gm__ half*)((uintptr_t)a + (1ULL << 48));`（error 第 15 行）vs `badPtr = a;`（fixed 第 15 行）
- 【约束】GM 物理地址空间 48bit，偏移 2^48 致地址 bit[48]≠0 超 48bit
- 【匹配模式】diff_pattern

### scalar_gm_addr_oob-RULE-02：asc_copy_gm2ub_sync 源 GM 地址超 48bit

- 【条件】`asc_copy_gm2ub_sync` 的 src `__gm__` 指针地址 bit[48]≠0
- 【API】`asc_copy_gm2ub_sync((__ubuf__ void*)xLocal, (__gm__ void*)badPtr, TILE_LENGTH * sizeof(half));`（error 第 17 行）
- 【约束】src badPtr 地址 bit[48]≠0 → SU SEQ 地址格式非法
- 【匹配模式】存在性检查

## 修复方案

**标准修复模式**：移除 GM 指针 2^48 偏移，使用合法基址。

```cpp
// 修复前（error）
__gm__ half* badPtr = (__gm__ half*)((uintptr_t)a + (1ULL << 48));
asc_copy_gm2ub_sync((__ubuf__ void*)xLocal, (__gm__ void*)badPtr, TILE_LENGTH * sizeof(half));

// 修复后（fixed）
__gm__ half* badPtr = a;
asc_copy_gm2ub_sync((__ubuf__ void*)xLocal, (__gm__ void*)badPtr, TILE_LENGTH * sizeof(half));
```

**防御性编程建议**：

- 禁止对 GM 指针叠加 ≥ 2^48 的偏移常量
- GM 指针运算前断言地址 < (1ULL << 48)

## 变体索引

| 变体 | 偏移表达式 | 地址 bit[48] | 结果 |
|------|-----------|-------------|------|
| 基准（本用例） | (1ULL << 48) | =1 | L107 GM>48bit |
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
| vec_gm_addr_oob (L159) | 同族 GM>48bit，VEC/Dcache 路径 |
| gm_addr_oob | 同族地址越界，GM-alloc-OOB 路径（越界 vs 格式非法） |
| scalar_addr_error (10) | 同族地址越界，UB OOB 路径 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01）、存在性检查（RULE-02）。
- **命中判定**：按下列步骤核对 RULE-01 的 GM 基址偏移差异；RULE-02 还需确认 error/main.asc 第 17 行 `asc_copy_gm2ub_sync` 使用该 badPtr 作为源地址。
  1. 读取 error/main.asc 第 15 行 `(__gm__ half*)((uintptr_t)a + (1ULL << 48))`。
  2. 判定偏移 2^48 致地址 bit[48]≠0 → 超 48bit GM 物理地址空间。
  3. fixed 版第 15 行 `badPtr = a`（合法基址）→ 安全。
  4. error 版 stderr 应含 EZ9999（验证阶段确认 errcode，L107 SEQ GM>48bit）。
  - `runtime_required=true`：L107 的运行时归属需 errcode 确认；仅有地址构造与 EZ9999 不能替代该确认。
- **修复映射**：RULE-01、RULE-02 → 移除 `1ULL << 48` 偏移，使用 `badPtr = a`，确保后续搬运读取合法 GM 基址。
