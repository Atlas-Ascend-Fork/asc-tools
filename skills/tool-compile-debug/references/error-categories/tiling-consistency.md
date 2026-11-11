# tiling_consistency — Tiling 字段 Host↔Kernel 不一致

## 根因

Ascend C 自定义算子的 Tiling 数据由共享结构体或 Host 注册定义等形式表达，Host 填充、Kernel 消费，
两侧的数据布局和字段使用必须一致。不一致时可能出现
`no member named` 或类型错误。报错位置在 Kernel 侧时，仍需同时核对定义、生成头与消费方，
不能直接判定应修改 Host 侧。

## 典型报错

```
add.asc:100:49: error: no member named 'tileLength' in 'AddTilingData'
kernel.cpp:35:20: error: no matching function for call to 'GET_TILING_DATA'
add_tiling.h:12:15: error: unknown type name 'TilingData'
```

> 注意：报错文件可能是生成的中间文件（如 `add_tiling.h`），不是用户手写的源文件。
> 需要找到真正的声明来源，如共享头、Host 注册定义或生成模板。

## 先确认本工程的数据传递形式

| 已有源码与构建证据 | 核对范围 |
|---|---|
| Host 构造具体结构体，以该值启动 Kernel，Kernel 形参也是同一结构体 | 对照字段及调用参数；直接使用已传入的值或同类型局部副本，不为套用地址解包宏改变入口或引入序列化 |
| Host 将共享结构体内容复制到设备缓冲，Kernel 接收 `GM_ADDR` 或带 `__gm__` 的结构体指针 | 保留设备地址传递，核共享布局、有效长度、地址空间和原消费方式；指针不等于已解包的值，不能把 Host 对象地址或引用当作设备缓冲 |
| Host 和 Kernel 使用共享裸结构体，Kernel 以 `REGISTER_TILING_DEFAULT` 等声明供当前构建框架识别 | 保留本工程有效的结构体及生成路径，核字段、结构名、可见性和实际注入头；注册算子工程不等于必须迁移到 Host 宏族 |
| Host 已使用 `BEGIN_TILING_DATA_DEF` / `REGISTER_TILING_DATA_CLASS`，并由生成头解包 | 核注册定义、序列化写入和生成后的消费布局；下文宏族示例只适用于这种形式 |

以上形式不能仅凭目录名或某个宏的存在互相替换。原构建能够提供声明时，不必先还原整个框架生成过程。
定义和受影响消费者已经解释局部命名或字段错配，就在现有形式内修改并编译；缺失总头的级联错误先按
[header-missing.md](header-missing.md) 处理，不预先重写 tiling 表示。只有真实错误或原任务约束要求改变表示时，
才进一步核对序列化、对齐、数据长度及各消费方。

### 用生产方和消费方判断是否需要解包

把受影响数据连成一条链：**Host 实际准备的值或缓冲 → 启动实参 → Kernel 形参类型 → 字段读取**。
这条链用于区分修改消费表达式与修复生成接口，不要求追查所有 Tiling 宏。现有链路确认为值传递时，
不因其他样例使用 GM 地址就改入口、增加设备分配或强制类型转换；现有链路确认为 GM 缓冲时，也不能为消除报错直接按值赋给结构体。
缓冲若是注册类序列化结果，还须按其生成布局读取，不能仅凭字段同名就当成共享裸结构体。

`GET_TILING_DATA` 调用中局部名字未声明，可能是当前编译单元没有提供预期宏；搜索未命中既不能证明 SDK 删除了该功能，也不能证明它是编译器内建。
若共享结构声明、Host启动实参和Kernel形参已共同证明是同类型按值传递，就直接读取该形参的字段，
或按原使用方式建立同类型局部副本，再用原目标验证。不为模拟缺失的`GET_TILING_DATA`新建同名兼容宏；
这会掩盖值与地址的区别，还可能与以后提供的框架宏冲突。无需先查完该宏的所有历史版本才能修正当前消费方式。
反之，任务使用生成解包接口而宏不可见、Host 确实写入序列化缓冲、或生成布局与消费字段不符时，
继续查生成头、注入参数及依赖是必要步骤，不能用自造同名宏或强转绕过。
需要预处理取证时使用实际编译单元的参数和头文件；脱离框架的探针查不到宏，只能说明该探针没有提供它。

## TilingData 宏族结构

以下示例说明已使用 Host 注册宏族的工程，不是所有 Tiling 数据的必选写法。

```cpp
// Host 侧定义（通常在 *_tiling.cpp 或 op_host/*.h 中）
namespace optiling {
BEGIN_TILING_DATA_DEF(AddTilingData)
    TILING_DATA_FIELD_DEF_STRUCT(Utils, utilsData)       // Utils 已按宏族定义
    TILING_DATA_FIELD_DEF(uint32_t, tileNum)
    TILING_DATA_FIELD_DEF(uint32_t, tileLength)
    TILING_DATA_FIELD_DEF_ARR(uint32_t, 8, dimBuf)        // 数组字段
END_TILING_DATA_DEF;

REGISTER_TILING_DATA_CLASS(Add, AddTilingData)             // 注册
} // namespace optiling
```

```cpp
// Kernel 侧消费（在 .asc / kernel 文件中）
GET_TILING_DATA(tilingData, tiling)                       // tiling 为传入的数据地址
// 需要显式类型且当前生成头提供该宏时：
// GET_TILING_DATA_WITH_STRUCT(AddTilingData, tilingData, tiling)
```

### 字段表示核对

以下是该宏族序列化字段的常用形式，实际类型支持和布局以当前 SDK 的宏定义及生成头为准；不用于要求裸结构体改写为宏族。

| 可用类型 | 说明 |
|---|---|
| 固定宽度整型 | 核对字段宽度、符号性与两侧声明，例如 `uint32_t` / `int32_t` / `uint64_t` |
| 浮点 | 按当前 SDK 核对支持范围，不能因 Host C++ 类型可编译就推定生成布局支持 |
| 嵌套结构体 | `TILING_DATA_FIELD_DEF_STRUCT(Utils, fieldName)`，被嵌套体也需 `BEGIN/END_TILING_DATA_DEF` 包裹 |
| 定长数组 | `TILING_DATA_FIELD_DEF_ARR(dtype, N, fieldName)` |
| **不可用** | `std::vector` / 指针 / `std::string` / 自定义 class / 虚函数 |

### 常见不一致形态

| 形态 | 报错表现 | 根因 |
|---|---|---|
| 字段名拼写不一致 | `no member named 'tileLength' in 'AddTilingData'` | Host 写了 `tileLen`，Kernel 用 `tileLength` |
| 字段类型不一致 | `no matching function` / 类型转换错误 | Host 定义 `uint32_t`，Kernel 期望 `int32_t` |
| 嵌套结构体字段缺失 | `no member named 'xxx' in struct Utils'` | 嵌套体内的字段在 Host 或 Kernel 侧漏定义 |
| `GET_TILING_DATA` 不可见或展开失败 | 宏未展开、展开后标识符未声明或调用不匹配 | 核本工程是否应由生成头提供该宏、结构体是否可见、生成步骤是否执行；按值结构体传参也可能根本不需要地址解包 |
| tiling_key 分支不一致 | 编译过了但运行时取到错误分支 | `TILING_KEY` 宏条件在两侧不一致 |

## 修法

1. **核对根因候选**：沿 `root_cause_localization.root_file` 或编译器位置检查定义与受影响使用点；只有涉及生成物时才沿其生成链和 include 顺序继续定位。定位输出只提供候选；根据原接口和真实声明决定修改定义、生成配置还是消费代码。生成头缺失或错名时，同时核它的生成源与受影响成员使用；同一生成契约的已知错配一起修正，再编译，不只补 include 留下已可见的字段错误。尚无错误证据的其他层不提前重构。

2. **核对字段名/类型/顺序**：对照本工程的共享结构体或 Host 注册定义、生成结构及 Kernel 使用点，逐项核字段名、类型、声明/序列化顺序和数组长度，保持原传递方式、布局与用途。不因结构名含Tiling就改Schema；不能把 Host 注册类当成设备裸结构体替换，也不为改名增加独立序列化定义。确有切分或缓冲设计缺口时，调用 CANNBOT `ascendc-tiling-design` 技能；局部错误不要求重新设计整个 Tiling。

3. **检查 tiling_key**：若算子有多分支，核对 Host 的 `context->SetTilingKey(...)` 与 Kernel 的 `TILING_KEY_IS(...)` 分支及构建选择。`AddConfig("ascend910b")` 等配置目标 SoC，不是注册 tiling_key 的接口。编译通过后分支选择仍错误属于后续功能验收问题。

4. **验证**：沿原构建配方真实编译验证。仅当改动确实改变接口契约时才同步 Schema；不因 Tiling 字段修复自动改动 `.json` 的 `input_desc`。出现新错误时回喂诊断。

## 常见根因补充

- 生成器输出的 tiling 头与手写 Host 代码不一致：先核对本次构建是否重新生成并 include 了正确的头。需要比较生成器输出时在独立目录进行，避免覆盖用户修改。
- 结构体 padding/sizeof 不一致：检查两侧对齐属性、序列化长度和生成头布局。依据现有接口恢复一致的布局，不能未经核实直接删除对齐约束。

## 说明

分类匹配由随包引擎的静态规则检查承担（`scripts/engine/harness/engine/static_rule_checker.py` 的 tiling 字段一致性检查，含 `tiling_field_mismatch` 类）。跨文件定位由 diagnose 入口内部承担（读其 `root_cause_localization` 产物）。
