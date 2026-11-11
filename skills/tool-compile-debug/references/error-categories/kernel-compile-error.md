# kernel_compile_error — Kernel 侧编译错误

## 根因

已确认属于算子**设备调用域**的编译错误，且不归入更具体的子类(tiling/api/template/header)。先按[调用域判定](README.md#host与kernel的判定依据)核首错函数、关联声明、有效宏及实际命令的参数和目标；`.asc`、op_kernel或编译器名称本身不证明Kernel归属。常见:
- kernel 用了不支持的 C++ 特性(unsupported_cpp_feature 类);
- kernel 内类型/intrinsic 用法错但不属于 API 改名/幻觉;
- kernel 抛异常(throw 在设备代码里不支持）。

## 典型报错（真实日志样貌）

```
error: ... is not supported in device code
error: cannot use 'throw' / exceptions in __aicore__ function
```
普通C++语义错误也可能发生在Kernel；混合编译单元和证据不足时保留unknown，不能把分类phase或编译器名称当作已核实的调用域。

## 修法

1. 从现有日志或原构建取得失败单元的实际命令与目标；位置或覆盖有缺口时才按需使用 diagnose/verify，不为取得工具产物重复编译。
2. 对异常、RTTI或库功能被当前设备编译拒绝的情形，核该代码承担的计算、错误传播与副作用，再选择目标支持且保留契约的实现。不能仅删除报错语句、改成静默返回或给Host功能加修饰就称修好。
3. 若错误其实是 API 名/签名问题，转 [api-incompatible.md](api-incompatible.md);
若是模板/重载，转 [template-error.md](template-error.md); 若是 tiling 字段，转 [tiling-consistency.md](tiling-consistency.md)。

## 说明

分类 `kernel_compile_error` 与建议提供检索线索；真实命令、声明、调用域和任务要求共同决定修法。

## 常见根因补充

- `calling a __host__ function("X") from a __aicore__ function("Y") is not allowed`：
核helper函数体及其继续调用的能力是否适用于目标设备。纯设备可用的计算helper缺限定时可按当前声明规则补齐；Host资源管理或系统接口应留在Host并传递所需结果，不能仅加 `__aicore__` 绕过调用域矛盾。
- `cannot initialize a variable of type '__gm__ T *' with an lvalue of type 'GM_ADDR'`：
核生产方地址空间、真实元素类型、长度与对齐；这些事实支持时才转换为对应GM指针。强转不搬运数据、不改变物理存储类型，也不证明地址有效。
- `unknown type name 'half'; did you mean 'half2'?`：先核实际入口、类型声明与目标条件。近名建议不证明两者的元素宽度、向量宽度、布局或重载语义等价；原契约需要half时应恢复其合法可见性，不能直接换成half2。
- `Induction variable must have a type uint16_t`：若当前调用确有该约束，核循环上界、步长、回绕和覆盖范围，再选合法变量类型或分块实现。不能截窄归纳变量后遗漏元素，或使循环无法退出。

## 历史诊断与条件建议

- `static assertion ... tque's depth is zero`：断言要求深度零使用 LocalTensor 引用式
AllocTensor/DeQue；深度零本身并非非法。按原资源和流水契约核调用形式，不能只为编过增加深度。
- `no matching member function for call to 'InitBuffer'` 不代表只传了一参。9.0 声明包含
TBuf 的 `(buf, lenBytes)`、队列的 `(que, num, lenBytes)` 和 tuple 地址形式；按实际对象、参数类型和原容量选择，不能猜缓冲数量或长度。
- `no template named 'is_same'; did you mean 'std::is_same'`：若原意是标准类型比较，
且声明与实参匹配，按使用位置选择 `std::is_same<T,U>` 类型或其 `::value` 布尔值，并显式包含 `<type_traits>`。单独补头不把 std 名字引入当前作用域；这也不是 kernel 专有错误。
- `use of undeclared identifier 'sqrtf'; did you mean 'sqrt'`：既有知识给出全局
device `float sqrt(float)`。当前调用域、类型及数值需求与该声明相符时可用全局 `sqrt`；不能自动加 `AscendC::` 或将此候选推广到其他类型/精度需求。
- `use of undeclared identifier 'Simt'` 只证明当前名字未解析。核目标、声明入口、namespace、
地址空间和调用层，不据此认定整套 SIMT 不可用，也不删除原计算。
- `FixpipeParams` 缺成员时先匹配目标的 Fixpipe 重载与参数类型。9.0 的
`FixpipeParamsV220` 有公开成员及五参构造函数；`{nSize, mSize, srcStride, dstStride, reluEn}` 是该构造的调用形式，不是聚合字段顺序。不能声称成员赋值语法不可用，或将 V220 参数强套到另一目标；必须保留布局、量化、dtype 与步长单位。
- `MatmulImpl` 未解析时，按当前 matmul_intf 的 `AscendC::Matmul` 别名、模板参数和
cube/vector 配置选择入口；`MulImpl` 近名建议不能替代矩阵乘。
- `SortConfig` 未解析不能反推目标为非 950。9.0 的 Host tiling 配置与 Device 配置字段
不同，需核入口和 guard。目标支持时修可见性；确实不支持原接口时，才按原排序方向、索引、dtype 和工作区契约选择基础 Sort。

> 声明存在不等于当前调用或修复已编译通过。
