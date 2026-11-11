# host_compile_error — Host 侧编译错误

## 根因

已确认属于算子 **Host调用域**的 C++ 编译错误，例如tiling、InferShape/InferDataType中的声明或调用问题。先按[调用域判定](README.md#host与kernel的判定依据)核函数、声明、有效宏和实际编译上下文，不能仅凭op_host目录或C++报错判为Host。常见:
- host 侧声明与定义签名不一致(decl/def mismatch 类);
- `ge::Tensor` / `gert::*` 等 host 侧类型用错或 include 不全;
- host 侧调用了 kernel 侧才有的符号。

## 典型报错（真实日志样貌）

```
error: conflicting declaration / no matching function for call to '...'
error: 'ge::Tensor' has no member named '...'
error: redefinition of '...' with different signature
```

分类映射的phase通常是`host_compile`，不是执行域的独立证明；相同C++诊断也可能来自Kernel。

## 修法

1. 检查 host include 路径与 `ge::Tensor` / `gert::TilingContext` 等签名是否匹配当前 CANN 版本。
2. decl/def mismatch:对齐声明(.h)与定义(.cpp)的参数类型/个数/const 限定。
3. 若 host 侧引用了 kernel 侧符号，把该逻辑挪回 kernel 或通过 tiling 结构体传递。

## 与相邻类的区别

- 首错函数和实际编译上下文已确认属于**设备调用域** → [kernel-compile-error.md](kernel-compile-error.md)。
- 报错是 **host↔kernel 的 tiling 字段对不上** → [tiling-consistency.md](tiling-consistency.md)。
- 报错是 **.json 算子描述与 host 签名对不上** → [schema-consistency.md](schema-consistency.md)。

## 说明

分类匹配与建议动作由随包引擎生成（分类 `host_compile_error`：检查 Host include 路径与 `ge::Tensor` 等签名）。

## 历史诊断与条件建议

- `use of undeclared identifier 'EVENT_IDn' / PIPE_FIX` 只说明当前名字不可见，不能由名字
推定函数域。先核实际函数和原同步用途：device 侧保持事件方向及 Set/Wait 关系；确为 host 同步时，按已有 stream/event 生命周期使用运行时接口。不能凭未声明诊断删除同步；只有已有证据证明引用无功能用途时才删除。该形态只是普通函数中失败探针的历史案例。
