# dcache_biu_response — Dcache BIU response 候选（GM 地址异常，访问通道待核实）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | dcache_biu_response |
| 硬件事件编号 | L113 |
| 异常类型 | Dcache BIU response 候选（GM 地址异常，访问通道待核实） |
| 需求目标 | AICoreError.md L113 Dcache BIU bus response错误 — EventID 0x80C98002 |
| 实际覆盖 | 尚未确认独立 L113 覆盖；文档现存调用片段为 asc_copy_gm2ub_sync，需原始源码及日志核对 |
| 错误码 | errcode:TBD（Dcache BIU 路径无先例，比较基准 scalar_gm_addr_oob 的 0x4000 族，预期 errorStr 不同） |
| 类别 | system_bus |
| 源文件 | error/main.asc:17（`badGm = (__gm__ half*)((uintptr_t)a + (1ULL << 48))`） |
| 报错模式 | EZ9999（验证阶段补充 retCode/errcode 字段） |
| 内存区域 | GM（GlobalMemory，bit[48]≠0 超 48bit 物理地址空间，256 TiB 上限） |
| API层级 | TC-Low |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 结构变更（badGm 构造段→直接 badGm=a）+ return 1→0 |
| behavior_note | GM>48bit 构造；文档中的 asc_copy 与历史 scalar 描述冲突，L113 归因待原始源码和日志确认 |

## 触发条件

**违规模式**：TC-Low 平坦 `__vector__` 函数（asc_simd），先经 asc_copy_gm2ub_sync 把合法 GM a 搬入 UB（建立流水线工作负载），再构造 `badGm = (__gm__ half*)((uintptr_t)a + (1ULL << 48))` 使 GM 地址 bit[48]≠0（超 48bit GM 物理地址空间）。现存片段用 `asc_copy_gm2ub_sync` 读取 badGm；本库未附原始种子，不能据此确认 Dcache→BIU 通道或独立 L113 错误。

**命名/语义说明**：scalar_gm_addr_oob 的文档也包含 asc_copy_gm2ub_sync，不能通过 detector 名称或本片段区分通道。两个候选须核对原始访问指令、目标架构和日志；相同 EZ9999/errcode 本身不能证明独立覆盖，也不自动删除候选。

**安全模式**：`badGm = a`（合法 GM 基址，无偏移）。

## 检测规则

### dcache_biu_response-RULE-01：GM 地址异常与访问调用核对（L113 归因待验证）

- 【条件】asc_copy_gm2ub_sync 读源 GlobalTensor 地址 bit[48]及以上非零，超出本文历史地址构造范围；是否经 Dcache→BIU 待验证
- 【API】`__gm__ half* badGm = (__gm__ half*)((uintptr_t)a + (1ULL << 48)); asc_copy_gm2ub_sync((__ubuf__ void*)xLocal, (__gm__ void*)badGm, TILE_LENGTH * sizeof(half));`
- 【约束】GM 物理地址宽度 48bit（256TB）；需核对目标地址格式与实际分配；仅凭 bit[48] 不能确认 Dcache→BIU response 或 L113
- 【匹配模式】存在性检查

### dcache_biu_response-RULE-02：badGm 构造段注入

- 【条件】badGm 通过 `baseAddr + (1ULL << 48)` 构造，替换原 a（合法基址）
- 【API】`__gm__ half* badGm = (__gm__ half*)((uintptr_t)a + (1ULL << 48));`（error）vs `__gm__ half* badGm = a;`（fixed）
- 【约束】error 版第 17 行构造 badGm（GM>48bit），fixed 版直接使用 a（合法）
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：删除 badGm 构造偏移，直接使用合法基址 a。

```cpp
// 修复前（error）
__gm__ half* badGm = (__gm__ half*)((uintptr_t)a + (1ULL << 48));

// 修复后（fixed）
__gm__ half* badGm = a;
```

**防御性编程建议**：

- GM 指针构造后断言 `(addr >> 48) == 0`
- 禁止对 GM 地址做超出 alloc 范围的位运算偏移
- asc_copy_gm2ub_sync 前 assert src GM 地址在 48bit 物理空间内

## 变体索引

| 变体 | GM 地址构造 | 访问通道 | 触发路径 |
|------|------------|---------|---------|
| 基准（本用例） | a + (1ULL<<48) | asc_copy，通道待核实 | L113 |
| 无偏移变体 | a（合法） | asc_copy，通道待核实 | 安全（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：GM 地址合法 | addr < 2^48 且在 alloc 范围内 | fixed 版（badGm = a） |
| EC2：GM 地址超 48bit | addr bit[48+] != 0 | error 版（badGm = a + (1ULL<<48)） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| scalar_gm_addr_oob (L107) | 同族 GM>48bit 注入；文档也含 asc_copy，需运行证据区分 |
| vec_gm_addr_oob (L61) | 同族 GM>48bit 注入，Vec asc_copy 路径 |
| fixpipe_biu_error (L85) | 同族 BIU 响应错误，FixPipe L0C→GM 写路径 |

## 检测执行指引

- **适用匹配模式**：存在性检查（RULE-01）、diff_pattern（RULE-02）。
- **命中判定**：按下列步骤核对 RULE-01 的非法 GM 地址及搬运调用、RULE-02 的 badGm 构造差异；L113 与 L107 的运行时区分和重复覆盖判别必须保留。
  1. 读取 error/main.asc 第 15 行 `asc_copy_gm2ub_sync(xLocal, a, ...)` 确认合法 GM 搬运建立流水线。
  2. 读取第 17 行 `__gm__ half* badGm = (__gm__ half*)((uintptr_t)a + (1ULL << 48))` 确认 GM>48bit 构造。
  3. 读取第 19 行 `asc_copy_gm2ub_sync(xLocal, badGm, ...)` 核对使用 badGm 的实际指令，不从函数名推导 Dcache 通道。
  4. 确认异常偏移使用于访问，并获取架构、分配及通道证据；L113 归因仍须日志验证。
  5. fixed 版第 17 行 `badGm = a`（合法基址）→ 安全。
  6. error 版 stderr 应含 EZ9999（验证阶段补充 retCode/errcode）。
  7. 运行后结合原始访问指令与日志比较 scalar_gm_addr_oob；不可区分时报告候选碰撞，不能仅凭相同 errcode 删除知识条目。
  - `runtime_required=true`：L113 根因规则在取得原始指令和通道日志前均不计分；GM 偏移差异只作为通用地址风险辅助证据，不能支撑独立 L113 覆盖。
- **修复映射**：RULE-01、RULE-02 → 删除 badGm 构造中的 `1ULL << 48` 偏移，恢复 `badGm = a`，后续搬运使用合法 GM 地址。
