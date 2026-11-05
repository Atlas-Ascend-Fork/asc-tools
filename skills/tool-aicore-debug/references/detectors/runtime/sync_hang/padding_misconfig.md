# padding_misconfig — padding / fmap / filter 配置不匹配

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | padding_misconfig |
| 异常类型 | kernel 挂起；历史 README 标记 mte_padding_cfg |
| 错误码 | 41（历史 README 声明；种子实测表现为挂起，不代表 stderr 实际返回 41） |
| 类别 | sync_hang |
| 源文件 | cann-aicore/09_loaddata3d/ld3d_padding_cfg_hangs/error/main.asc |
| 报错模式 | kernel_hangs |
| 内存区域 | L1 |
| kernel_diff_type | kernel_diff |
| behavior_note | error_code=41 与 exit_code=124 为两种独立线索，不应从超时推导错误码 |

## 触发条件

历史种子在 dav-2201 环境下挂起，外层 `timeout` 可产生进程退出码 124。当前源码须分别核对目标 API 的 padding 字段范围和 fmap/filter 几何条件，不能从“padding 很大”直接推出挂起。

对普通对称 padding：`H + 2*pad >= filterH` 且 `W + 2*pad >= filterW`。增加非负 pad 不会减小这个范围；没有普遍成立的 `pad <= H/2` 限制。非对称 padding 须分别使用 left/right、top/bottom。目标 API、dilation 等改变有效 filter 尺寸时，先按该重载计算有效尺寸再比较。约定见 `references/constraints/semantic_contract.md`。

## 检测规则

### padding_misconfig-RULE-01：padding 字段超出已确认的合法范围

**【条件】**pad 超过当前架构和 API 文档允许的上限，或赋值截断后改变了实际配置
**【API】**LoadData3D / LoadData3DParamsV2 padding 字段
**【约束】**各 padding 值均处于已确认的字段范围内；上限不能从 fmap 尺寸的一半推断
**【来源】**LLM 推断
**【匹配模式】** 值域判断

### padding_misconfig-RULE-02：有效 fmap 小于 filter

**【条件】**已知 H/W、padding、有效 filter 尺寸，计算结果为 `H + 2*pad < filterH` 或 `W + 2*pad < filterW`
**【API】**LoadData3D padding / fmap / filter 参数
**【约束】**两个方向均满足有效 fmap 不小于 filter，使用宽整数计算
**【来源】**LLM 推断
**【匹配模式】** 值域判断

### padding_misconfig-RULE-03：tiling 驱动的参数组合违反约束

**【条件】**tiling 传入 padding/fmap/filter，在实际赋值后违反 RULE-01 或 RULE-02 的约束
**【API】**Host tiling → LoadData3D 参数赋值
**【约束】**追溯实际值并校验两个方向；仅出现 tiling 字段名不足以确认违规
**【来源】**LLM 推断
**【匹配模式】** 值域判断

### padding_misconfig-RULE-04：联合校验缺失（辅助规则）

**【条件】**LoadData3D 调用前没有字段范围和 fmap/filter 联合校验
**【API】**LoadData3D 调用前参数校验
**【约束】**加入联合校验可及早发现非法配置；缺少校验本身不证明挂起根因，不计异常命中率
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

### padding_misconfig-RULE-05：超时监控缺失（辅助规则）

**【条件】**执行环境没有可用的有界等待和失败退出机制
**【API】**Host 运行器 / watchdog
**【约束】**设置超时并保留日志；超时返回不等于 Device 任务已终止，清理按运行环境支持的机制执行；不计异常命中率
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. 超出字段范围：按目标 API 允许的值修正 tiling；不能仅通过缩小类型或截断掩盖错误。
2. 有效 fmap 小于 filter：先确认目标算子语义，再调整 shape/filter/tiling 或提出 padding 候选；若必须保留原 padding，报告配置不可行。
3. 所需 padding 下限超过 API 上限：返回失败，禁止对倒置区间调用 clamp。
4. 缺失校验/监控：补充防御措施，不能将它们作为已确认根因自动改变计算参数。

### 代码模板

以下为 Host 侧、对称 padding、已知有效 filter 尺寸的纯参数检查和候选计算。`maxAllowedPad` 必须来自目标 API/算子约束；候选值仅用于评审，不自动写入 tiling。

```cpp
#include <algorithm>
#include <cstdint>

inline bool ValidatePaddingConfig(uint32_t h, uint32_t w, uint32_t pad,
                                  uint32_t filterH, uint32_t filterW,
                                  uint32_t maxAllowedPad) {
    return h > 0 && w > 0 && filterH > 0 && filterW > 0 &&
           pad <= maxAllowedPad &&
           uint64_t(h) + 2ULL * pad >= filterH &&
           uint64_t(w) + 2ULL * pad >= filterW;
}

inline bool ProposeSymmetricPadding(uint32_t h, uint32_t w,
                                    uint32_t filterH, uint32_t filterW,
                                    uint32_t requestedPad, uint32_t maxAllowedPad,
                                    uint32_t& proposedPad) {
    if (h == 0 || w == 0 || filterH == 0 || filterW == 0) return false;
    uint64_t needH = filterH > h ? (uint64_t(filterH) - h + 1) / 2 : 0;
    uint64_t needW = filterW > w ? (uint64_t(filterW) - w + 1) / 2 : 0;
    uint64_t minPad = std::max(needH, needW);
    if (minPad > maxAllowedPad) return false;
    uint64_t candidate = std::min<uint64_t>(
        std::max<uint64_t>(requestedPad, minPad), maxAllowedPad);
    proposedPad = static_cast<uint32_t>(candidate);
    return ValidatePaddingConfig(h, w, proposedPad, filterH, filterW, maxAllowedPad);
}
```

### 防御性编程建议

- 覆盖奇数差值、H/W 约束不同、不可行区间及 uint32_t 边界。
- 保留报错/超时日志，核对是否存在同步类竞争候选。
- 浮点值或外部 shape 无法静态确定时标注 `runtime_required=true`，不臆造参数。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 历史挂起种子 | cann-aicore/09_loaddata3d/ld3d_padding_cfg_hangs | 挂起已记录；具体字段/几何根因须复核原始参数与日志 | EC3 |
| filter 高度过大 | 参数示例 | H=12, filterH=15, pad=1 | EC2 |
| 两方向约束不同 | 参数示例 | H=12, W=8, filterH=15, filterW=15，需 pad>=4 | EC2 |
| 字段范围不可行 | 参数示例 | H=W=1, filterH=filterW=100, maxAllowedPad=10 | EC2 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1 | 参数几何与字段范围均合法 | ValidatePaddingConfig 返回 true；不代表其他异常均排除 |
| EC2 | 字段或几何约束违反 | 有已确认的参数值和 API 范围 |
| EC3 | 待验证 | 参数/范围缺失或仅有超时症状 |

## 关联异常

- flag_mismatch / alloc_free_mismatch / enque_deque_mismatch：同为挂起候选，依控制流配对证据区分。
- fmap_size_illegal / first_window_overflow / l1_3d_size_illegal：同属 LoadData3D 参数检查。
- load3d_mte_param_check：静态预防检查须采用相同几何条件，不能与本 detector 相互矛盾。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/03）、存在性检查（RULE-04/05）。
- **命中判定**：按目标重载提取实际字段范围及有效 filter/fmap/padding，计算几何条件。参数或上限不可确定时标记 `runtime_required=true`；跨边界无法定值时标记 `trace_limited=true`。RULE-04/05 是辅助信息。仅超时或历史码 41 不足以确认具体根因。
- **修复映射**：RULE-01 → 标准模式 1；RULE-02/03 → 标准模式 2/3；RULE-04/05 → 标准模式 4。需要更改计算参数时先证明与算子语义相容，否则只输出候选方案。
