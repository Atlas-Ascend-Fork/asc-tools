# compiler_limit — 触发编译器限制

## 根因

编译器报告容量限制或在不同优化选项下行为不同，是继续定位的线索；它们不能单独证明源码合法、没有未定义行为、数值正确或确属编译器缺陷。常见形态:
- 跳转超范围(`out of jump/jumpc imm range`):函数太大/分支太多，跳转立即数越界;
- 巨型 switch / 循环展开(huge_switch_or_unroll 类);
- 模板递归深度超限(template_depth_limit 类);
- 优化敏感:`-O0` 过、`-O2/-O3` 挂。

## 典型报错（真实日志样貌）

```
error: out of jump/jumpc imm range
... jump ... range ...
```
这些消息可触发现有`compiler_limit`分类；兼容`phase=kernel_compile`不是实际后端pass证明，仍须结合本次命令、源码位置和完整日志解释。

## 修法

1. **拆函数**:把巨型函数/switch 拆成多个小函数，缩短单个跳转跨度。
2. **降展开**:减少循环展开因子，或用运行时循环替代编译期展开。
3. **优化档位**:若`-O0`过而`-O2/-O3`挂，先核各次实际命令、源码与其他选项是否相同，
将结果记作优化选项敏感线索。按需使用`--run-probes`，不能只凭probe名称或解释字段确认真实执行。降优化属于配方变更；保留原配方失败证据，另验规避后的功能、原数值判据和适用的性能要求，不把低优化编过改签为原配方已修复。
4. 模板深度：加显式特化/降递归层数，或调 `-ftemplate-depth`。

## 与 compiler_internal 的区别

- `compiler_limit`:编译器报告限制(jump range / depth)；重构、减少展开等是待验证的候选，不能据此确认代码合法。
- `compiler_internal`:观察到编译器崩溃线索(ICE/segfault/stack dump)；先核进程来源和更早错误，再缩小复现。
见 [compiler-internal.md](compiler-internal.md)。

## 说明

匹配规则见随包引擎 `scripts/engine/harness/knowledge/error_signatures.yaml`（`jump_range` 等）；优化敏感判定对比不同优化等级的探针结果。

## 判定参考数据

buffer或tile限制先按[模板资源约束](template-error.md#tilestage和容量约束要一起满足)核当前实例的实际占用、对齐及流水关系；不能仅改到断言不再触发就认定运行正确。

编译超限类（buffer 超上限 / tiling 装不下）先用硬件参数速查核对量级：L0A/L0B 跨代一致各 64KB；UB/L0C 随架构不同（L0C 2201=128KB → 3510=256KB）。精确值读 `${ASCEND_HOME_PATH}/<arch>/data/platform_config/*.ini` 或运行时 `GetCoreMemSize`。详见 [../knowledge/arch-migration-2201-3510.md](../knowledge/arch-migration-2201-3510.md)。
