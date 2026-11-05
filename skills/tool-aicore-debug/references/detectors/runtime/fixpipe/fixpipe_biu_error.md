# fixpipe_biu_error — FixPipe 访问 BIU 异常（L0C→GM 写超 48bit 物理地址空间）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixpipe_biu_error |
| 硬件事件编号 | L85 |
| 异常类型 | FixPipe 访问 BIU 异常（L0C→GM 写 DMA 通道经 BIU，dst 地址超 48bit） |
| 错误码 | errcode:TBD（L85 fixp_biu_rdwr_resp 路径无先例；比较基准 GM>48bit 三例的 0x4000 族） |
| 类别 | fixpipe |
| 源文件 | error/main.asc:80-85（badGm 构造）+ :91（`Fixpipe(badGm, c1d, fp)`） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] |
| 内存区域 | GM（GlobalMemory，bit[48]≠0 超 48bit 物理地址空间，256 TiB 上限） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 结构变更（badGm 构造段→直接 Fixpipe(cGM,...)）+ return 1→0 |
| behavior_note | 复用 W5a 骨架（合法 srcStride/dstStride），第 4 级 Fixpipe 处构造 badGm=baseAddr+(1ULL<<48) 使 FixPipe L0C→GM 写 DMA 经 BIU 访问 >48bit 非法地址 |

## 触发条件

**违规模式**：完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe），前 3 级合法。第 4 级 Fixpipe 构造 `badGm`：以 `cGM.GetPhyAddr()` 为基址，经 `reinterpret_cast<uintptr_t>(baseAddr) + (1ULL << 48)` 使 bit[48] 置位（超 48bit GM 物理地址空间），`badGm.SetGlobalBuffer(...)` 重绑后 `Fixpipe(badGm, c1d, fp)`。FixPipe 的 L0C→GM 写 DMA 经 BIU 访问 >48bit 非法地址 → 瞄准 L85 fixp_biu_rdwr_resp。

**安全模式**：`Fixpipe(cGM, c1d, fp)`（写合法 cGM，无 badGm 构造段）。

## 检测规则

### fixpipe_biu_error-RULE-01：Fixpipe dst GlobalTensor 地址超 48bit 物理空间

- 【条件】Fixpipe 写目标的 GlobalTensor 地址 bit[48]及以上非零，超 48bit GM 物理地址空间
- 【API】`GlobalTensor<float> badGm; badGm.SetGlobalBuffer(reinterpret_cast<__gm__ float*>(reinterpret_cast<uintptr_t>(baseAddr) + (1ULL << 48)), cSize); Fixpipe(badGm, c1d, fp);`
- 【约束】GM 物理地址宽度 48bit（256TB）；bit[48]=1 → 超出物理空间 → BIU 响应错误
- 【匹配模式】存在性检查

### fixpipe_biu_error-RULE-02：badGm 构造段注入

- 【条件】Fixpipe 调用前插入 badGm 构造段（GetPhyAddr + 偏移 + SetGlobalBuffer），替换原 cGM
- 【API】`Fixpipe(badGm, c1d, fp);`（error）vs `Fixpipe(cGM, c1d, fp);`（fixed）
- 【约束】error 版 80-85 行构造 badGm（GM>48bit），fixed 版直接使用 cGM（合法）
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：删除 badGm 构造段，Fixpipe 直接写合法 cGM。

```cpp
// 修复前（error）
GlobalTensor<float> badGm;
auto baseAddr = cGM.GetPhyAddr();
badGm.SetGlobalBuffer(
    reinterpret_cast<__gm__ float*>(
        reinterpret_cast<uintptr_t>(baseAddr) + (1ULL << 48)),
    static_cast<uint64_t>(cSize));
Fixpipe(badGm, c1d, fp);

// 修复后（fixed）
Fixpipe(cGM, c1d, fp);
```

**防御性编程建议**：

- Fixpipe dst GlobalTensor 构造后断言 `(addr >> 48) == 0`
- 禁止对 GM 地址做超出 alloc 范围的位运算偏移

## 变体索引

| 变体 | GM 地址构造 | 写目标 | 访问通道 |
|------|------------|--------|---------|
| 基准（本用例） | baseAddr + (1ULL<<48) | badGm | FixPipe L0C→GM 写（经 BIU） |
| 无偏移变体 | cGM（合法） | cGM | FixPipe L0C→GM 写（合法） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：Fixpipe dst GM 地址合法 | addr < 2^48 且在 alloc 范围内 | fixed 版（Fixpipe(cGM,...)） |
| EC2：Fixpipe dst GM 地址超 48bit | addr bit[48+] != 0 | error 版（badGm = baseAddr+(1ULL<<48)） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| mte_biu_rdwr_resp (L22) | 同族 BIU 响应错误，MTE 读通道路径 |
| scalar_gm_addr_oob (L107) | 同族 GM>48bit 注入，scalar 解引用路径 |
| vec_gm_addr_oob (L61) | 同族 GM>48bit 注入，Vec asc_copy 路径 |
| dcache_biu_response (L113) | 同族 GM>48bit 注入，Dcache 读路径（重复覆盖风险源） |

## 检测执行指引

- **适用匹配模式**：存在性检查（`fixpipe_biu_error-RULE-01`）；diff_pattern（`fixpipe_biu_error-RULE-02`）。
- **命中判定**：按 RULE-01 定位 Fixpipe 目标地址超 48bit 的构造，并按 RULE-02 对照 badGm 构造段及目标替换差异；实际是否覆盖 L85 仍需提取运行时签名，与本文列出的 GM>48bit 基线区分。保留以下定位、修复对照和重复覆盖判别步骤：

  1. 读取 error/main.asc 第 80-85 行 badGm 构造段，确认 `reinterpret_cast<uintptr_t>(baseAddr) + (1ULL << 48)`（GM>48bit）。
  2. 确认第 91 行 `Fixpipe(badGm, c1d, fp)`（写非法 badGm）。
  3. fixed 版第 85 行 `Fixpipe(cGM, c1d, fp)`（写合法 cGM）→ 安全。
  4. error 版 stderr 应含 EZ9999 + retCode=0x26 [aicore exception]。
  5. 运行后提取 errcode/subErrType/biu error info，与 GM>48bit 三例（scalar_gm_addr_oob / vec_gm_addr_oob / dcache_biu_response）比对判别重复覆盖（若 errcode 完全相同 (0,0x4000,0) 且不可区分 → 判重复覆盖剔除；若 L85 特征 → 存活）。
- **修复映射**：
  - `fixpipe_biu_error-RULE-01`、`fixpipe_biu_error-RULE-02` → 删除 GetPhyAddr、超范围偏移和 SetGlobalBuffer 组成的 badGm 构造段，改为 `Fixpipe(cGM, c1d, fp)`，直接写入合法 cGM。
