# ccu_stack_check — CCU 拦截 / 编译器优化（Dcache stack / 禁止 UB）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | ccu_stack_check |
| 异常类型 | 静态预防（CCU 拦截 / 编译器优化） |
| 错误码 | 无（CCU 拦截先于 data_exception / 编译器优化为大数组） |
| 类别 | static_preventive |
| 性质 | 预防型 |
| 覆盖条目 | C-4：L148 SCALAR 禁止 UB / L149 MTE 禁止 UB / L160 Dcache stack 越界（3 条） |
| 检测时机 | 静态代码扫描 / host 侧监控 |
| 报错模式 | 无（v13 实机：CCU OOB 兜底 / 编译器优化为 UB 分配） |

## 触发条件

CCU 地址检查拦截先于 data_exception 上报，或超大 local 变量被编译器优化为 UB 分配。

- **违规模式**：SCALAR/MTE 直接访问 UB 保留区域（offset < 0 或 >= UB_SIZE）/ kernel 中超大 local 变量超过 stack 预算
- **安全模式**：避免访问 UB 保留区域 / 大数组改用 `__ubuf__` 声明

## 检测规则

### ccu_stack_check-RULE-01：SCALAR 禁止 UB 空间（L148）

【条件】SCALAR 路径访问 UB offset < 0 或 >= UB_SIZE
【API】SCALAR store/load 指令（UB 地址）
【约束】SCALAR 访问 UB offset ∈ [0, UB_SIZE)；CCU 地址检查拦截先于 data_exception
【匹配模式】值域判断

### ccu_stack_check-RULE-02：MTE 禁止 UB 空间（L149）

【条件】MTE 路径访问 UB offset < 0 或 >= UB_SIZE
【API】MTE store/load 指令（UB 地址）
【约束】MTE 访问 UB offset ∈ [0, UB_SIZE)；CCU 拦截
【匹配模式】值域判断

### ccu_stack_check-RULE-03：Dcache stack 越界（L160）

【条件】kernel 中 local 变量总大小超过 stack 预算（~4KB），超大 local 被编译器优化为 UB 分配
【API】kernel local 变量声明
【约束】local 变量总大小 <= stack 预算；大数组改用 `__ubuf__` 声明
【匹配模式】值域判断

## 修复方案

### 标准修复模式

1. L148/L149：避免 SCALAR/MTE 直接访问 UB 保留区域
2. L160：将大数组改为 `__ubuf__` 声明（UB 分配而非 stack）

### 代码模板

```cpp
// 预防型：L148/L149 UB 访问边界校验
__aicore__ inline void SafeScalarUBStore(
    LocalTensor<float>& ubBuf, int32_t offset, float value) {
    // L148: SCALAR 禁止 UB 保留区域
    ASSERT(offset >= 0 && static_cast<uint32_t>(offset) < UB_SIZE_FLOATS);
    ubBuf.SetValue(offset, value);
}
```

```cpp
// 预防型：L160 大数组改用 __ubuf__ 声明
// 违规（stack 溢出，编译器优化为 UB）：
// __aicore__ void BadKernel() {
//     float bigArray[4096];  // 超 stack 预算，编译器优化
// }
// 安全：
__aicore__ void GoodKernel() {
    __ubuf__ float bigArray[4096];  // 显式 UB 分配
}
```

### 防御性编程建议

1. 监控 CCU 地址检查异常 `(0, 0x4000, 0)`，作为 L148/L149 的前置拦截信号
2. 静态扫描 kernel 中 local 变量总大小，警告超过 ~4KB 的情况
3. SCALAR/MTE 路径的 UB 访问统一加 ASSERT 边界校验

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| SCALAR 禁止 UB | C-4 L148 | SCALAR store UB 下溢/上溢，CCU 拦截 | EC1 |
| MTE 禁止 UB | C-4 L149 | MTE 访问 UB 保留区域，CCU 拦截 | EC2 |
| Dcache stack 越界 | C-4 L160 | 超大 local 变量，编译器优化 | EC3 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1 | SCALAR UB 访问合法 | offset ∈ [0, UB_SIZE) |
| EC2 | MTE UB 访问合法 | offset ∈ [0, UB_SIZE) |
| EC3 | stack 使用合法 | local 总大小 <= 4KB |

## 关联异常

- **scalar_ub_forbidden** / **vec_ub_forbidden_access**（runtime）：L148/L149 的运行时表现（CCU OOB 兜底）
- **dvg_stack_overflow**（runtime）：L160 的 stack 溢出表现（MPU OOB 兜底）

## 检测执行指引

- **适用匹配模式**：值域判断（全部 RULE）
- **命中判定**：offset 越界 / local 变量超预算 → 命中
- **修复映射**：RULE-01/02 → UB 访问 ASSERT；RULE-03 → 改用 `__ubuf__`
- **trace_limited 判定**：local 变量跨函数累积统计 → `trace_limited=true`
- **runtime_required 判定**：offset 值依赖运行时计算 → `runtime_required=true`
