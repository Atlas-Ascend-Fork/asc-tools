# fixpipe_param_check — FixPipe 参数穷尽不检出

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixpipe_param_check |
| 异常类型 | 静态预防（FixPipe 参数穷尽后硬件不检出） |
| 错误码 | 无（参数穷尽后硬件静默，OOB 兜底） |
| 类别 | static_preventive |
| 性质 | 预防型 |
| 覆盖条目 | C-4：L77 fixp b4/ch_split 非法配置 / L80 FIXP l1 写地址溢出（2 条） |
| 检测时机 | host tiling 计算 / kernel 编译期 |
| 报错模式 | 无（v12/v13 实机验证参数穷尽后硬件不报目标错误，OOB 兜底） |

## 触发条件

FixPipe 参数穷尽后 dav-2201 硬件不检出，或非法值致 hang / 被忽略。

- **违规模式**：`FixpipeParamsV220` 字段非法值（unitFlag 不在 {0,1,2,3} / isChannelSplit 非 F32 输出）/ 目标重载的实际写入区间超过目标分配（不能用源 srcNdStride 推导目标写址）
- **安全模式**：按 ISA 文档约束设置 FixpipeParamsV220 字段值

## 检测规则

### fixpipe_param_check-RULE-01：fixp b4/ch_split 非法配置（L77）

【条件】`FixpipeParamsV220.unitFlag` 不在 {0,1,2,3}，或 `isChannelSplit=true` 用于非 F32 输出类型
【API】`FixpipeParamsV220`（unitFlag / isChannelSplit 字段）
【约束】`unitFlag ∈ {0,1,2,3}`；`isChannelSplit=true` 仅 F32 输出可用
【匹配模式】值域判断

### fixpipe_param_check-RULE-02：FIXP l1 写地址溢出（L80）

【条件】目标 API 确认输出位于 L1，且按目标布局换算的写入字节区间超过其实际分配
【API】`Fixpipe`（输出位置、目标布局/步长、块数、写入区间）
【约束】`dst_offset_bytes + dst_span_bytes <= dst_allocation_bytes`；srcNdStride 是源参数，不能当作目标写步长；输出位置/布局不明时 runtime_required=true
【匹配模式】值域判断

## 修复方案

### 标准修复模式

1. 按 ISA 文档约束设置 FixpipeParamsV220 字段值
2. host 侧 tiling 计算添加 l1 写地址累积校验

### 代码模板

```cpp
// 预防型：FixpipeParamsV220 字段值域校验（L77）
void ValidateFixpipeParams(const FixpipeParamsV220& fp, DataType outputType) {
    // L77: unitFlag 合法性
    ASSERT(fp.unitFlag == 0 || fp.unitFlag == 1 || fp.unitFlag == 2 || fp.unitFlag == 3);
    // L77: isChannelSplit 仅 F32 可用
    if (fp.isChannelSplit) {
        ASSERT(outputType == DT_FLOAT && "isChannelSplit only valid for F32 output");
    }
}
```

```cpp
// 预防型：L1 写地址累积校验（L80）
bool ValidateFixpipeL1Write(uint64_t offsetBytes, uint64_t writeSpanBytes,
                              uint64_t allocationBytes) {
    // writeSpanBytes 须先按已确认的目标 API 布局计算。
    return offsetBytes <= allocationBytes && writeSpanBytes <= allocationBytes - offsetBytes;
}
```

### 防御性编程建议

1. tiling 计算函数返回前调用 `ValidateFixpipeParams` + `ValidateFixpipeL1Write`
2. 编译期约束 FixpipeParamsV220 字段枚举值

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| b4 N 非对齐 | C-4 L77 | unitFlag 非法致 hang 或被忽略 | EC1 |
| ch_split 非 F32 | C-4 L77 | isChannelSplit 用于非 F32 | EC2 |
| l1 写地址溢出 | C-4 L80 | 已确认目标写区间超分配 | EC3 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1 | unitFlag 合法 | ∈ {0,1,2,3} |
| EC2 | isChannelSplit 合法 | 仅 F32 输出时 true |
| EC3 | L1 写合法 | 目标写入字节区间处于实际分配内 |

## 关联异常

- **fixp_b4_misalign** / **fixp_ch_split_illegal**（runtime）：L77 的运行时表现（OOB 兜底）
- **fixp_l1_write_overflow**（runtime）：L80 的运行时表现（OOB 兜底）
- **fixpipe_cfg_illegal** / **fixpipe_l1_overflow**（runtime）：FixPipe 参数类异常

## 检测执行指引

- **适用匹配模式**：值域判断（全部 RULE）
- **命中判定**：字段或目标范围违反已确认的约束时命中；仅缺 host 校验属于防御建议，不证明目标异常
- **修复映射**：RULE-01 → ValidateFixpipeParams；RULE-02 → ValidateFixpipeL1Write
- **trace_limited 判定**：参数来自 host→kernel 传播链 → `trace_limited=true`
- **runtime_required 判定**：参数值依赖运行时 tiling → `runtime_required=true`
