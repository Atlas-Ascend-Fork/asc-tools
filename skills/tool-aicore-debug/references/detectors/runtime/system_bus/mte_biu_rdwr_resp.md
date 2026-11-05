# mte_biu_rdwr_resp — MTE BIU_RDWR_RESP（BIU 读写响应错误）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | mte_biu_rdwr_resp |
| 硬件事件编号 | L22 |
| 异常类型 | MTE BIU_RDWR_RESP（BIU 读写响应错误） |
| 错误码 | errcode:TBD |
| 类别 | system_bus |
| 源文件 | error/main.asc:18-23（预热回写 + badGm 构造 + 4 次连续读） |
| 报错模式 | EZ9999 + retCode=0x31 [vector core exception] |
| 内存区域 | GM（GlobalMemory，bit[48]≠0 超物理地址空间） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 结构变更（预热回写+for loop+badGm 构造→单次合法读）+ return 1→0 |
| behavior_note | GM>48bit 构造 + 4 次连续 MTE 读加重 BIU 读响应路径压力 |

## 触发条件

**违规模式**：先经 asc_copy_gm2ub_sync 合法搬入 UB 建立流水线，再 asc_copy_ub2gm_sync 预热回写加重 MTE 管线压力。构造 `badGm = (__gm__ half*)((uintptr_t)a + (1ULL << 48))` 使 GM 地址 bit[48]≠0（超 48bit GM 物理地址空间）。连续 4 次（BAD_COPY_TIMES）asc_copy_gm2ub_sync 从 badGM 读取，加重 MTE 读响应路径压力，尝试路由到 BIU_RDWR_RESP（L22）。GM>48bit 本身即保证 EZ9999（BIU response error）。

**安全模式**：`badGm = a`（合法 GM 地址，无偏移）+ 单次 asc_copy_gm2ub_sync（无 for loop）+ 移除预热回写。

## 检测规则

### mte_biu_rdwr_resp-RULE-01：GM 地址超 48bit 物理空间

- 【条件】`__gm__` 指针地址 bit[48]及以上非零，超 48bit GM 物理地址空间
- 【API】`__gm__ half* badGm = (__gm__ half*)((uintptr_t)a + (1ULL << 48))`
- 【约束】GM 物理地址宽度 48bit（256TB）；bit[48]=1 → 超出物理空间 → BIU 响应错误
- 【匹配模式】存在性检查

### mte_biu_rdwr_resp-RULE-02：连续多次 MTE 读加重 BIU 读响应路径

- 【条件】for loop 多次 asc_copy_gm2ub_sync 从非法 GM 读取
- 【API】`for (int i = 0; i < BAD_COPY_TIMES; i++) { asc_copy_gm2ub_sync(..., badGm, ...); }`；BAD_COPY_TIMES=4
- 【约束】4 次连续读加重 BIU 读响应路径压力，提高 BIU_RDWR_RESP 命中率
- 【匹配模式】API 调用模式

### mte_biu_rdwr_resp-RULE-03：预热回写加重 MTE 管线双向压力

- 【条件】在注入前先执行 asc_copy_ub2gm_sync 预热回写
- 【API】`asc_copy_ub2gm_sync((__gm__ void*)c, (__ubuf__ void*)xLocal, ...)`
- 【约束】预热回写加重 MTE 管线 UB→GM 方向压力，与 GM→UB 读形成双向压力
- 【匹配模式】API 调用模式

## 修复方案

**标准修复模式**：使用合法 GM 地址，移除 for loop 和预热回写。

```cpp
// 修复前（error）
asc_copy_ub2gm_sync((__gm__ void*)c, (__ubuf__ void*)xLocal, TILE_LENGTH * sizeof(half));
__gm__ half* badGm = (__gm__ half*)((uintptr_t)a + (1ULL << 48));
for (int i = 0; i < (int)BAD_COPY_TIMES; i++) {
    asc_copy_gm2ub_sync((__ubuf__ void*)xLocal, (__gm__ void*)badGm, TILE_LENGTH * sizeof(half));
}

// 修复后（fixed）
__gm__ half* badGm = a;
asc_copy_gm2ub_sync((__ubuf__ void*)xLocal, (__gm__ void*)badGm, TILE_LENGTH * sizeof(half));
```

**防御性编程建议**：

- GM 指针构造后断言 `(addr >> 48) == 0`
- 禁止对 GM 地址做超出 alloc 范围的位运算偏移

## 变体索引

| 变体 | GM 地址构造 | 连续读次数 | 预热回写 |
|------|------------|-----------|---------|
| 基准（本用例） | a + (1ULL<<48) | 4 | 有 |
| 无预热变体 | a + (1ULL<<48) | 4 | 无 |
| 单次读变体 | a + (1ULL<<48) | 1 | 无 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：GM 地址合法 | addr < 2^48 且在 alloc 范围内 | fixed 版（badGm=a） |
| EC2：GM 地址超 48bit | addr bit[48+] != 0 | error 版（+(1ULL<<48)） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| dcache_biu_response | 同骨架 `__vector__` + asc_copy_gm2ub_sync + GM>48bit 路径，机制重复覆盖风险 |
| fixpipe_biu_error | 同族 BIU 响应错误，Fixpipe 路径 |

## 检测执行指引

- **适用匹配模式**：存在性检查（RULE-01）、API 调用模式（RULE-02、RULE-03）。
- **命中判定**：按下列步骤核对 RULE-01 的非法 GM 地址、RULE-02 的连续多次读取和 RULE-03 的预热回写；并与 dcache_biu_response 比对运行时证据，保留重复覆盖判别。
  1. 读取 error/main.asc 第 19 行 `badGm = (...a + (1ULL << 48))` 确认 GM>48bit。
  2. 确认第 18 行预热回写 asc_copy_ub2gm_sync 存在。
  3. 确认第 21-23 行 for loop 4 次连续读 badGm。
  4. fixed 版 `badGm = a`（合法）+ 单次读 + 无预热 → 安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x31 [vector core exception]。
  6. 运行后提取 errcode/subErrType/biu error info，与 dcache_biu_response 比对判别重复覆盖。
  - `runtime_required=true`：BIU 读写响应归属需 errcode/subErrType/biu error info 确认；预热和多次读取仅提供本篇注入路径证据。
- **修复映射**：RULE-01 → 恢复 `badGm = a`；RULE-02 → 移除 for loop，恢复单次合法读取；RULE-03 → 移除注入前的预热回写，采用修复方案中的正常搬运路径。
