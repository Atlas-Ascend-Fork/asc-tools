# arch_api_guard — 架构/API 限制守卫

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | arch_api_guard |
| 异常类型 | 静态预防（架构/API 限制） |
| 错误码 | 无（API 不可用 / 编译器禁止 / 字段守卫） |
| 类别 | static_preventive |
| 性质 | 预防型 |
| 覆盖条目 | C-1：L73 isToUB 守卫 / L81 文档明确不检出 / L120 编译器禁止递归 / L161 无 SetUBSize（4 条） |
| 检测时机 | 编译期 / 静态代码扫描 |
| 报错模式 | 无（编译器拦截或硬件不检出） |

## 触发条件

dav-2201 上因 API 不可用、字段守卫、编译器禁止或无封装而无法触发的错误。

- **违规模式**：在 dav-2201 上使用不可用 API（如 `FixpipeConfig.isToUB`）、递归调用 `__aicore__` 函数、或调用不存在的 `SetUBSize`
- **安全模式**：避开不可用 API；递归改迭代；UB 容量由目标环境决定置

## 检测规则

### arch_api_guard-RULE-01：isToUB 字段在 dav-2201 不可用（L73）

【条件】代码使用 `FixpipeConfig.isToUB = true`，但 `__NPU_ARCH__ != 3510 && != 5102`
【API】`FixpipeConfig.isToUB`
【约束】`isToUB` 仅在 `__NPU_ARCH__==3510||5102` 下存在；dav-2201（2201）上字段不存在
【匹配模式】存在性检查

源码证据：`kernel_struct_fixpipe.h:41-43`

### arch_api_guard-RULE-02：Fixpipe out 写地址溢出不报（L81）

【条件】`Fixpipe()` 调用的 dst GM 地址范围超过 GM 分配大小
【API】`Fixpipe`（dst GM 参数）
【约束】目标写入字节区间须在实际 GM 分配内，不能把源 srcNdStride 当作目标写步长；历史资料未检出目标 L81，可能观察到替代信号 `fixp_biu_rdwr_resp`（L85）；当前信号须由日志确认
【匹配模式】值域判断

### arch_api_guard-RULE-03：__aicore__ 函数递归调用（L120）

【条件】`__aicore__` 修饰的函数中出现直接/间接递归调用
【API】`__aicore__` 函数调用
【约束】AscendC kernel 不支持递归调用；bisheng 编译器报 `error: recursive aicore function call is not allowed`
【匹配模式】存在性检查

### arch_api_guard-RULE-04：无公开 SetUBSize API（L161）

【条件】代码调用 `SetUBSize()` 或假设 UB size 可动态配置
【API】`SetUBSize`（不存在）/ `GetUBSizeInBytes()`（getter 也被 `5102/3003/3113/3510` 守卫，不含 2201）
【约束】不得假设可通过 SetUBSize 改变 UB 容量；实际可分配容量须按当前架构、软件栈和分配证据确认，参见 references/constraints/semantic_contract.md
【匹配模式】存在性检查

源码证据：`kernel_operator_sys_var_intf.h:66-79`

## 修复方案

### 标准修复模式

1. **L73**：避免在 dav-2201 上使用 Fixpipe 写 UB 路径；改用 DataCopy（MTE 管线）写 UB
2. **L81**：确保 按目标输出布局核对 Fixpipe GM 写入区间，不使用源 stride 推导目标地址；监控 `fixp_biu_rdwr_resp`（L85）作为替代信号
3. **L120**：将递归逻辑改为迭代；AscendC kernel 不支持递归调用
4. **L161**：优化 UB 使用，通过 TQue buffer 复用减少峰值占用（UB 容量由目标环境决定）

### 代码模板

```cpp
// L120 修复：递归改迭代
// 违规（编译器拒绝）：
__aicore__ inline void Recurse(int n) {
    if (n > 0) Recurse(n - 1);  // 递归 aicore 函数，bisheng 报错
}
// 安全：
__aicore__ inline void Iterate(int n) {
    for (int i = 0; i < n; i++) { /* ... */ }  // 迭代，编译通过
}
```

```cpp
// L161 修复：UB buffer 复用（固定 size）
// 违规（假设可配 UB size）：
// SetUBSize(256 * 1024);  // API 不存在
// 安全：
// 通过 TQue buffer 复用减少峰值占用，容量按目标环境确认
```

### 防御性编程建议

1. 编译期约束：`static_assert(__NPU_ARCH__ != 2201, "isToUB not available on dav-2201")`（L73）
2. Fixpipe dst GM 大小断言：按目标布局计算写入区间后检查 `dstOffsetBytes <= gmBufferSize && dstSpanBytes <= gmBufferSize - dstOffsetBytes`（L81）
3. 静态扫描 `__aicore__` 函数中的递归调用（L120）
4. 不假设 UB size 可配；用 `GetUBSizeInBytes()` 在支持的架构上查询（L161）

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| isToUB 守卫 | C-1 L73 | dav-2201 使用 isToUB 字段 | EC1 |
| Fixpipe out 不报 | C-1 L81 | dst GM 超限，硬件静默 | EC2 |
| 递归调用禁止 | C-1 L120 | __aicore__ 函数递归 | EC3 |
| 无 SetUBSize | C-1 L161 | 调用不存在的 SetUBSize | EC4 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1 | dav-2201 上使用 3510/5102 守卫字段 | `__NPU_ARCH__==2201` + isToUB 用法 |
| EC2 | Fixpipe dst 超限且无替代信号监控 | dst 范围 > buffer |
| EC3 | __aicore__ 函数递归 | 静态调用图含环 |
| EC4 | 调用 SetUBSize 或假设 UB 可配 | API 名匹配 |

## 关联异常

- **fixpipe_biu_error**（runtime）：L81 的替代信号 L85（`fixp_biu_rdwr_resp`）
- **fixpipe_l1_overflow** / **fixpipe_gm_overflow**（runtime）：Fixpipe 参数类溢出

## 检测执行指引

- **适用匹配模式**：存在性检查（RULE-01/03/04）、值域判断（RULE-02）
- **命中判定**：源码出现被守卫 API / 递归调用 / 不存在 API → 命中
- **修复映射**：RULE-01 → 改用 DataCopy；RULE-02 → buffer 大小断言；RULE-03 → 递归改迭代；RULE-04 → buffer 复用
- **trace_limited 判定**：递归调用图跨文件分析时标记 `trace_limited=true`
- **runtime_required 判定**：L81 dst GM 超限依赖运行时 tiling → `runtime_required=true`
