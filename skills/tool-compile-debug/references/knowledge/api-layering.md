# Ascend C API 分层模型

Ascend C 的接口按编程抽象层级分为多层，同短名 API 在不同层的签名/可用性/语义不同。
拿到模板/重载错误时，先确认当前代码调用的是哪层，再决定「换哪层接口」还是「修正当前层的用法」。

## 层级总表

| 层 | 入口头 | 命名空间/形态 | 典型误用 | 适用场景 |
|---|---|---|---|---|
| **Tensor 高层** | `kernel_operator.h`（聚合） | `AscendC::API名` | 从 Reg/SIMT 层按短名调高层 API | 向量/矩阵计算（大多数算子开发） |
| **vec 类（中高层）** | `basic_api/vec_*.h` | `AscendC::` + vec 前缀方法 | 与 Reg 层混淆 | 精细向量操作 |
| **Reg（MicroAPI）** | `adv_api/reg_*.h` | `AscendC::Reg::` | 用 Tensor 层的短名调 Reg API | 950 寄存器级编程 |
| **SIMT 标量** | `simt_api/cpp/*.h`（仅 3510） | 标量接口（无 AscendC:: 前缀） | 在 910 目标上使用 SIMT | 950 线程级编程 |
| **c_api（底层 C）** | `kernel_capi.h` 等 | C 函数（无命名空间） | 与 C++ API 混调 | 极底层优化 |

## 同短名跨层不可互换

同名 API 在不同层的行为不同，不能仅按短名替换：

| 短名 | Tensor 层 | Reg 层 | SIMT 层 |
|---|---|---|---|
| `Exp` | `AscendC::Exp`（向量级，输出 Tensor） | `AscendC::Reg::Exp`（寄存器级） | 标量 `exp()`（线程级） |
| `Add` | `AscendC::Add`（向量级） | 不同签名 | 不同签名 |

> `MicroAPI = Reg` 是别名关系，但该别名有目标宏条件——不能从别名文本推断任意目标可见。

## 编译报错与分层判别

| 报错形态 | 可能原因 | 核对方向 |
|---|---|---|
| `no matching function for call to 'AscendC::X'` | 调了错误层的 API | 核当前 include 了哪个层的头、实参类型是否匹配该层签名 |
| `candidate function not viable` | 该层有同名但签名不符 | 读候选列表的拒绝原因，核实参 dtype/形状/调用层 |
| `use of undeclared identifier 'X'` | 目标 arch 不支持该层 | SIMT 头仅 3510；核 `__NPU_ARCH__` |
| `no member named 'X' in namespace 'AscendC'` | 聚合头未导出该层入口 | 核逐头 include；调用 CANNBOT `ascendc-docs-search` 技能来查完整声明 |

## 本机核实命令

```bash
# 查 API 在哪层定义
grep -rn "API名" "$ASCEND_HOME_PATH/latest/ascendc/include" --include="*.h" | head -10

# 查该头被哪个聚合头 include
grep -rn "文件名.h" "$ASCEND_HOME_PATH/latest/ascendc/include" --include="*.h" | head

# 查 __NPU_ARCH__ 门控条件
grep -B3 -A3 "__NPU_ARCH__" "$ASCEND_HOME_PATH/latest/ascendc/include/目标头.h"
```

调用 CANNBOT `ascendc-docs-search` 技能来查完整 API 文档与示例代码。
