# dependency_missing — 依赖/生成头/include 目录缺失

## 根因

构建未能提供当前消费点所需的文件或目标，可能涉及include搜索路径、生成步骤、依赖注册或源文件缺失。
报错位置和文件名不能单独证明哪个环节有错；Tiling头既可能手写，也可能生成，应以当前工程规则为准。
本页与[头文件判别](header-missing.md)配合使用，不把两类原因按报错字样强行分开。

## 典型报错（真实日志样貌）

```
fatal error: 'auto_gen_xxx_tiling.h': No such file or directory
```
该消息只证明本次编译未找到指定文件；include拼写、实际搜索路径和文件生产方式仍须核对。

```
No such file or directory
```

## 修法

1. 沿报错include、实际编译命令和工程来源找到所需文件。现有手写共享头应恢复正确名称、位置或可见性，不能按`*_tiling.h`后缀重建。
2. 已证实是生成输出时，核生成输入、命令、输出路径及消费目标的依赖顺序；先处理生成步骤的真实失败。生成成功但消费者找错位置时修搜索/依赖关系，不重复生成整个工程。
3. include目录确实漏配时按现有目标恢复相应配置，不添加整棵SDK目录来掩盖语言路由错误。只有构建确实从已安装算子包消费该依赖时才核OPP路径；它不是所有生成头的默认来源。
4. 确为陈旧生成物时，保留用户修改并按[生成契约](schema-consistency.md)更新受影响部分。文件不存在但没有生成规则时继续核其职责；移除无用途残留需要消费与副作用证据，不能先删工程或运行生成器覆盖手写实现。

## 说明

`dependency_missing` 不是诊断输出的独立分类——此类失败在诊断结果里主判为 `header_missing` / `cmake_config` / `link_error`，本页提供跨类别的复合成因指引 （生成步骤/include 目录/依赖目标）；生成相关提示参考 `scripts/engine/harness/knowledge/fix_hints.yaml`。
