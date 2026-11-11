# link_error — 链接错误

## 根因

本次链接命令找不到符号定义或所需库。链接报错不能单独证明所有目标已完成编译，先核此前阶段的结果。常见线索：
- `undefined symbol` / `undefined reference`：定义缺失、未参与链接、符号名或可见性不符，也可能涉及库顺序和链接选项；
- `cannot find -lxxx`：请求的库名、搜索目录或库文件形态不符合本次链接命令；
- Host 引用的入口符号与生成物实际导出的符号不一致。

## 典型报错（真实日志样貌）

```
ld.lld: error: undefined symbol: some_missing_symbol()
bisheng: error: linker command failed with exit code 1 (use -v to see invocation)
```
```
undefined reference to '...'
cannot find -lxxx
```

匹配规则:`undefined reference to` / `cannot find -l|No such file or directory.*\.so` → category `link_error`,phase `link`。

> 工程可能使用 lld 或 GNU ld 等链接器；实际工具以本次命令为准。`ld.lld: error: undefined symbol:` 与 `undefined reference to` 均是引擎识别的线索。

## 修法

1. 保存失败链接命令，核请求的原始符号与对应目标文件或库的导出。按工程工具链使用 `nm`、`readelf` 或相应符号工具，区分定义未编译、未链入、名字修饰、可见性和链接选项问题。
2. 对 `cannot find -lxxx`，核实际 `-L` 搜索目录、sysroot、请求库名及可用库文件。按工程现有方式修 `target_link_libraries`、导入目标、`find_library` 或目标级搜索目录；不要把设置 `LD_LIBRARY_PATH` 当作直接 `-l` 库搜索的通用修法。
3. 运行时加载器找不到库时，另核 `RPATH`/`RUNPATH` 与实际运行环境的 `LD_LIBRARY_PATH`。这种加载失败不等于链接失败，按原任务需要交接运行期排查。
4. 修改后保留原链接目标、库选择和必要选项，沿原构建配方验证；不能以删除入口调用或省略库功能消除报错。

生成器或配置过程加载Host共享库时也可能暴露未定义符号。库文件存在、编译或生成共享库返回0，
并不证明加载时所需符号全部可解析。沿请求符号对照本工程跨编译单元的声明、定义及实际导出，
特别检查namespace、参数类型、引用及所指对象的const限定、C/C++链接约定。名字相似不代表同一符号；
按语义统一声明与定义，能由共享头约束时使用共享声明，不用盲加 `extern "C"` 或库目录掩盖签名差异。
修复后验证原配置/生成入口也能完成，不能仅以单独编译 `.so` 代替。

移除缺失的依赖或调用前，区分生成输出、独立编译单元、已被其他源码包含的实现，以及承担注册/
初始化的库。结果未使用、名字可疑或搜索未命中，都不能单独证明调用没有副作用。
当前声明、真实消费关系和任务要求支持其为无用途残留时才移除；必要实现应恢复生成或正确链接，
不能用空定义满足符号。

## 说明

分类 `link_error` 与建议只提供排查线索；修法须由实际链接命令、符号和依赖证据决定。

## 常见根因补充

- `undefined reference to 'aclrtlaunch_*'`：先核生成头、生成物是否参与链接以及导出符号。只有实际导出采用 C 链接，而调用方手写声明产生了 C++ 名字修饰时，才按真实声明修正链接约定；优先使用工程生成的声明，不盲目给所有入口增加 `extern "C"`。
