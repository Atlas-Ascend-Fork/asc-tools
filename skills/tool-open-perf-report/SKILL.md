---
name: tool-open-perf-report
description: >
  批量打开 Ascend C Toolkits 插件生成的性能分析报告（.rep / .ncrep / .npu-rep），
  在 IDE 的 profiling-report 查看器中每份报告一个标签页。当用户要求"批量打开性能分析报告"、
  "打开本次/某次运行的所有报告"、"同时打开这几份报告"或"对比多份报告"时使用。
---

# 批量打开性能分析报告

报告可视化由插件内建的 profiling-report 查看器承载，对外入口是一条 VSCode 命令（一次一份）。
"批量"就是把清单逐份串行调用：同一路径已打开时会自动前置复用，不会重复开窗。

## 第一步：收集报告清单

只认三种扩展名：`.rep`、`.ncrep`、`.npu-rep`。`csv` / `bin` / `json` 是报告的附属数据文件，
不走本技能。按以下顺序收集，高优先来源命中后不再向下扫描：

1. **用户直接给出的路径**（优先）：逐个校验文件存在、扩展名匹配，不匹配的剔除并向用户说明。
2. **用户指定的运行目录**：上板/仿真分析每次运行写到 `<workspaceRoot>/<ideDir>/opprof/<runId>/`，
   递归收集其中三种扩展名的文件。
3. **默认取最新运行**：未指定时扫描 `<workspaceRoot>/.vscode/opprof/`（CodeArts 下为 `.arts/opprof/`），
   按修改时间取最新的 1~3 个 `<runId>` 子目录（份数按用户要求调整）。
4. **外部添加的报告**：记录在 IDE 配置目录下的 `cache.json`（VS Code 为 `<workspaceRoot>/.vscode/cache.json`，
   CodeArts 为 `<workspaceRoot>/.arts/cache.json`）的 `fileMapLists.simulate` / `fileMapLists.on_board`
   （缓存键前缀 `simulate_` / `onboard_`），需要时一并纳入；读取前先校验该文件存在且为合法 JSON 结构。

执行前先向用户列出将要打开的清单；路径一律转成绝对路径。

## 第二步：逐份打开

### 方式 A（首选）：调用插件命令

命令 ID 固定为 `ascend-c-toolkit.openProfilingReport`：

```js
// 在具备"执行 VSCode 命令"能力的 Agent 工具中运行
for (const p of reportPaths) {
  try {
    await vscode.commands.executeCommand('ascend-c-toolkit.openProfilingReport', p); // 参数：绝对路径字符串，也接受 vscode.Uri
  } catch (e) {
    // 记录失败，继续下一份；不要让单份失败中断整批
  }
}
```

- `.npu-rep` 会自动路由到自定义编辑器 `opdevtoolsNpuRepViewer`；`.rep` / `.ncrep` 打开为 webview 面板。
- 已打开的同路径报告只前置（reveal），不会重复加载。
- 逐份 `await` 串行调用；大报告解析耗时，不要并发轰炸。

### 方式 B（终端兜底，仅 `.npu-rep`）

`*.npu-rep` / `*.npu.rep` 的自定义编辑器是默认编辑器（`priority: "default"`），
所以用宿主 IDE 的命令行直接打开即可批量生效：

```bash
code <报告1.npu-rep> <报告2.npu-rep> ...
```

`.rep` / `.ncrep` 没有注册默认编辑器，终端方式只会按文本打开二进制内容，**必须走方式 A**。

## 第三步：汇报结果

向用户报告：成功打开 N 份（列出文件名）、失败 M 份及原因（文件缺失 / 扩展名不符 / 命令未注册）。

## 注意事项

- Problems 面板里的性能提示只跟随**当前激活**的报告标签页；批量打开后同屏仅激活那份的提示生效，
  切换标签自动换源，关闭标签自动清空。
- 报告文件较大时打开有解析等待，属正常现象。
- 本技能只负责"把报告呈现在 IDE 里"；报告内容分析是 `tool-perf-analysis` 技能的职责。

## 兜底

| 情形 | 处理 |
|------|------|
| 打开命令未注册（插件未装/未激活） | 提示安装 Ascend C Toolkits 插件后重试 |
| `.npu-rep` 且无 IDE 环境（纯 CLI / CI） | 改用 `npu-rep-html` 技能生成自包含 HTML，用浏览器查看 |
| 工作区里找不到任何报告 | 先确认 `<workspaceRoot>/.vscode/opprof/` 是否存在；必要时提示用户先用插件跑一次上板/仿真分析 |
