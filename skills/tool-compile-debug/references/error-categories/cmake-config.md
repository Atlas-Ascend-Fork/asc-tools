# cmake_config — CMake 配置与构建规则问题

## 根因

配置、生成或执行构建规则时，工具链模块、目标与依赖无法按工程配方解析。常见线索：
- 采用 ASC 语言的工程未正确加载所需包或编译器模块；
- 工程声明的语言、工具链文件或包路径与所选安装不一致；
- 自定义 target 或生成文件的依赖规则缺失。

`No rule to make target` 通常由 make 执行构建规则时报告，不能仅凭它断言失败发生在 CMake configure 阶段；根因仍可能来自配置或生成链。

## 典型报错（真实日志样貌）

```
CMake Error: Could not find cmake module file: CMakeDetermineASCCompiler.cmake
CMake Error: Error required internal CMake variable not set, cmake may not be built correctly.
```
```
CMake Error at CMakeLists.txt:NN (find_package): Could not find a package configuration file provided by "ASCEND"
```
```
No rule to make target '...'
```

随包规则（`scripts/engine/harness/knowledge/error_signatures.yaml`）把 `CMake Error at|CMake Error:` 标为 `phase=cmake_configure`，把 `No rule to make target` 标为 `phase=cmake_generate`。这些是分类线索；另读 `failure_stage` 的证据与原始日志，不能把类别映射当作真实执行阶段的证明。

## 修法

1. 沿原命令检查源码目录、构建目录、生成器、toolchain 文件和 CMake 参数。若工程采用 ASC 语言，核其约定的 `find_package(ASC REQUIRED)` 或相应包入口及 `project(... LANGUAGES ASC)`；不要把这一形式强加给采用其他构建入口的工程。
2. 核实际安装中的模块位置和 CMake 查找路径；确需环境脚本时加载用户指定安装提供的 `set_env.sh`，不替换原工具链或追加猜测路径。
3. 对 `No rule to make target`，读完整的缺失目标和 `needed by` 依赖链，核文件路径、大小写、生成命令及依赖注册。源文件漏加入、生成头未产出、旧构建目录引用失效路径均需各自证据，不能统一归因于漏写 `target_link_libraries`。
4. 修正已确认的配置或依赖问题后，在独占副本沿原 target 和参数重新构建，保存本轮配置、生成和编译日志。

### 配置传播与嵌套失败

修复参数时沿现有配置入口 → 目标属性/生成命令 → 实际编译或链接参数核传播。
工程已有可配置架构、SDK路径或库选择时，修错误引用及覆盖关系，不把当前机器上有效的值写死来
替代配置入口。只有工程本来限定固定目标且没有这类接口时，才可按既定约束保留字面量；不为通用性
擅自新增支持目标。当前目标构建通过只证明该配置，其他目标是否保持可配置与是否已实测分别说明。

外层 `cmake` 失败也可能发生在它执行的Host编译、生成器或动态加载阶段。先读内层首个失败命令和
诊断：未定义符号时转[链接与动态加载](link-error.md)，源码错误沿对应声明处理。
不能仅因包装信息是CMake Error或opbuild失败，就重装环境或反汇编工具；局部声明/定义与实际符号
已经解释矛盾时，先修工程契约，再用原入口验证。

## 说明

分类匹配与建议动作由随包引擎生成（分类 `cmake_config`）。
