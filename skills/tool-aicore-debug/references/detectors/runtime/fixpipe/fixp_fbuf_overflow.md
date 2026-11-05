# fixp_fbuf_overflow — FIXP fbuf 读/写溢出（FixPipe fixed-point buffer）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixp_fbuf_overflow |
| 硬件事件编号 | L82/83 |
| 异常类型 | FIXP fbuf 读/写溢出（FixPipe fixed-point buffer） |
| 错误码 | errcode:TBD（待运行验证） |
| 类别 | fixpipe |
| 源文件 | error/main.asc:32-33（fbufEnd 构造 + `asc_copy_l12fb(fbufEnd, aL1, 1, 255, 0, 0)`）+ :24/:35（L0C OOB 兜底） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] |
| 内存区域 | fbuf（FixPipe fixed-point buffer，dav-2201 容量 = 1024×7 = 7168 字节 = 7KB） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | TC-Low 结构变更（fbufEnd+FBUF_END_OFFSET→fbufBase 无偏移 + burst_len 255→1 + cBad→cL0c）+ return 1→0 |
| behavior_note | asc_copy_l12fb 写 1×255×32B=8160B 从偏移 6656B 起 → 6656+8160=14816B > 7168B fbuf 容量 → 溢出 7648B；OOB 兜底 cBad=cL0c+32640 致 Mmad 写跨 L0C 边界 → L13 |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

**违规模式**：TC-Low 平坦 `__cube__` 函数流水线（asc_copy_gm2l1_nd2nz → asc_copy_l12l0a/l0b → asc_copy_l12fb → asc_mmad）。fbuf 指针经 `asc_get_phy_buf_addr(0)` 获取基址，dst 偏移至 fbuf 末尾附近（`+FBUF_END_OFFSET=6656B`），`asc_copy_l12fb(fbufEnd, aL1, 1, 255, 0, 0)` 写 `burst_num(1) × burst_len(255) × 32B = 8160B` 从偏移 6656B 起 → 14816B > 7168B fbuf 容量 → L82/83 fbuf 溢出。OOB 兜底：`cBad = cL0c + C_BAD_OFFSET(32640)` 致 Mmad 写跨 L0C 128KB 边界 → L13。触发顺序：fbuf 溢出先于 Mmad OOB。

**安全模式**：`fbufBase = asc_get_phy_buf_addr(0)`（无偏移）+ `asc_copy_l12fb(fbufBase, aL1, 1, 1, 0, 0)`（burst_len=1，32B 在 7168B 内）+ `asc_mmad(cL0c, ...)`（正常地址）。

## 检测规则

### fixp_fbuf_overflow-RULE-01：asc_copy_l12fb dst 偏移 + 写量溢出 fbuf 容量

- 【条件】asc_copy_l12fb 的 dst 指针偏移 + burst_num×burst_len×32B > fbuf 容量 7168B
- 【API】`__fbuf__ void* fbufEnd = (__fbuf__ void*)((uintptr_t)asc_get_phy_buf_addr(0) + FBUF_END_OFFSET); asc_copy_l12fb(fbufEnd, (__cbuf__ void*)aL1, 1, 255, 0, 0);`
- 【约束】fbuf 容量 = 7168B（7KB，dav-2201）；FBUF_END_OFFSET=6656B，burst_num=1, burst_len=255 → 写量=1×255×32B=8160B；6656+8160=14816B > 7168B → 溢出 7648B
- 【匹配模式】值域判断

### fixp_fbuf_overflow-RULE-02：FBUF_END_OFFSET + burst_len 放大注入

- 【条件】dst 偏移至 fbuf 末尾（6656B = 7168-512）且 burst_len=255 放大写量
- 【API】`fbufEnd = asc_get_phy_buf_addr(0) + 6656; asc_copy_l12fb(fbufEnd, aL1, 1, 255, 0, 0);`（error）vs `fbufBase = asc_get_phy_buf_addr(0); asc_copy_l12fb(fbufBase, aL1, 1, 1, 0, 0);`（fixed）
- 【约束】6656B 距 fbuf 上界 7168B 仅 512B 余量；burst_len=255 → 8160B >> 512B 余量 → 必溢出
- 【匹配模式】diff_pattern

### fixp_fbuf_overflow-RULE-03：asc_mmad c_matrix 偏移 OOB 兜底（L13）

- 【条件】`cBad = cL0c + C_BAD_OFFSET(32640)` 致 asc_mmad 写跨 L0C 128KB 边界
- 【API】`__cc__ float* cBad = cL0c + C_BAD_OFFSET; asc_mmad(cBad, aL0a, bL0b, M, N, K, 0, false, false, false);`
- 【约束】L0C 容量 = 32768 floats（128KB）；32640+256=32896 > 32768 → 越界 128 floats（OOB 兜底）
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：fbuf 写址恢复基址（无偏移），burst_len 恢复 1（32B 在 7168B 内），Mmad 写址恢复 cL0c（无偏移）。

```cpp
// 修复前（error）
__cc__ float* cBad = cL0c + C_BAD_OFFSET;
__fbuf__ void* fbufEnd = (__fbuf__ void*)((uintptr_t)asc_get_phy_buf_addr(0) + FBUF_END_OFFSET);
asc_copy_l12fb(fbufEnd, (__cbuf__ void*)aL1, 1, 255, 0, 0);
asc_mmad(cBad, aL0a, bL0b, M, N, K, 0, false, false, false);

// 修复后（fixed）
__fbuf__ void* fbufBase = (__fbuf__ void*)asc_get_phy_buf_addr(0);
asc_copy_l12fb(fbufBase, (__cbuf__ void*)aL1, 1, 1, 0, 0);
asc_mmad(cL0c, aL0a, bL0b, M, N, K, 0, false, false, false);
```

**防御性编程建议**：

- asc_copy_l12fb 前断言 `offset + burst_num × burst_len × 32 <= FBUF_SIZE`（7168B）
- fbuf 指针偏移做范围检查 `[0, FBUF_SIZE - transfer_bytes]`
- 禁止 burst_num=255, burst_len=255（2MB 同时溢出 fbuf 与 L1）

## 变体索引

| 变体 | FBUF_END_OFFSET | burst_num×burst_len | 写量 | 越界量 |
|------|----------------|--------------------|---------|--------|
| 基准（本用例） | 6656B | 1×255 | 8160B | +7648B（>7168B） |
| 安全变体 | 0 | 1×1 | 32B | 0（fixed 版） |
| 禁止变体 | 6656B | 255×255 | ~2MB | 溢出 fbuf + L1（双错误源） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：fbuf 写址安全 | offset + transfer_bytes ≤ 7168B | fixed 版（0+32=32） |
| EC2：fbuf 写溢出 | offset + transfer_bytes > 7168B | error 版（6656+8160=14816） |
| EC3：L0C OOB 兜底 | cBad 偏移 + M×N > 32768 floats | error 版（OOB 兜底 L13） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| cube_fixp_buffer_overflow (L13) | 同族 FixPipe buffer 越界，TC-Low loop 累积路径 |
| cube_l0c_overflow (L13) | 同族 L0C 写越界，本用例 OOB 兜底路径 |
| cube_fixp_overflow | 同族 fixp 溢出路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（`fixp_fbuf_overflow-RULE-01`）；diff_pattern（`fixp_fbuf_overflow-RULE-02`）；diff_pattern（`fixp_fbuf_overflow-RULE-03`）。
- **命中判定**：按 RULE-01 计算 fbuf 写入范围，按 RULE-02 核对偏移和 burst_len 注入差异，按 RULE-03 单独记录 L0C OOB 兜底；区分先执行的 fbuf 写入与后执行的 Mmad，实际 L82/83 覆盖仍需下述签名判别。保留以下取值、修复对照和运行签名核对步骤：

  1. 读取 error/main.asc 第 13 行 `FBUF_END_OFFSET = 6656`，第 32-33 行 fbufEnd 构造 + asc_copy_l12fb burst=1,255。
  2. 计算 6656 + 1×255×32B = 14816B > fbuf 容量 7168B → fbuf 溢出 7648B（L82/83）。
  3. 读取第 12 行 `C_BAD_OFFSET = 32640`，第 24 行 `cBad = cL0c + C_BAD_OFFSET`，第 35 行 `asc_mmad(cBad,...)` → L0C OOB 兜底。
  4. fixed 版 fbufBase（无偏移）+ burst 1,1（32B）+ asc_mmad(cL0c,...) → 安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x26。
  6. 签名判别：若与 R1 OOB 基线（cube error info=0x30, errcode=(0,0x4000000000000000,0), errorStr="CUBE illegal"）整体一致 → 判仅兜底；若出现 fbuf/FixPipe 特征 → 命中 L82/83。
- **修复映射**：
  - `fixp_fbuf_overflow-RULE-01`、`fixp_fbuf_overflow-RULE-02` → 将 fbuf 写址恢复为 `asc_get_phy_buf_addr(0)` 基址，调用恢复为 `asc_copy_l12fb(fbufBase, aL1, 1, 1, 0, 0)`（按原修复示例保留指针类型转换），使写入量为 32B。
  - `fixp_fbuf_overflow-RULE-03` → 移除 cBad 的 C_BAD_OFFSET 偏移，改为 `asc_mmad(cL0c, ...)`，同时消除 L13 兜底。
