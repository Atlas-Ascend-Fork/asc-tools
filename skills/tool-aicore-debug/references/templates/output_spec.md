# 输出契约 — 字段词典与渲染规则

本文件是 diagnose 报告字段、必填条件与渲染行为的权威定义。先生成 JSON，再按下述规则生成 Markdown；`output_json.md` 仅提供 JSON 示例，`output_md.md` 仅提供渲染骨架。报告承载当前环境版本、候选修改方法与修改原因。

## Schema

```ts
type Report = {
  env: {
    hardware: string;       // NPU 型号，如 "Atlas A2" / "昇腾 910B"
    cann: string;           // CANN / Ascend C 软件栈版本
    skill: string;          // tool-aicore-debug 自身版本
    runtime: string;        // 编译器 / aarch64 / kernel build 指纹
  };
  candidates: Array<{
    id: string;             // 异常 ID
    category: "runtime" | "static_preventive";  // 候选来源类别
    nature?: "preventive" | "trusted" | "index" | "exploratory";  // 仅 static_preventive 必填；runtime 留空
    fix: {
      reason: string;       // 修改原因（融合命中细节 + 规则依据，一句话）
      code_blocks: Array<{  // brief、证据不足、无安全修复或说明性候选时为空数组 []
        title: string;      // 修复要点
        language: string;   // "cpp" / "ascendc"
        before: string;     // 违规代码（多行，保留缩进）
        after: string;      // 修复代码（多行，保留缩进）
      }>;
    };
  }>;
};
```

## 字段词典（JSON ↔ MD）

| JSON 路径 | 类型 | 含义 | MD 呈现 |
|-----------|------|------|---------|
| `env.hardware` | string | NPU 型号（Atlas A2 / 昇腾 910B 等） | "环境"行 |
| `env.cann` | string | CANN / Ascend C 软件栈版本 | "环境"行 |
| `env.skill` | string | tool-aicore-debug 自身版本 | "环境"行 |
| `env.runtime` | string | 编译器 / aarch64 / kernel build 指纹 | "环境"行 |
| `candidates[].id` | string | 异常 ID | 候选标题 |
| `candidates[].category` | enum | `runtime` / `static_preventive`（候选来源类别） | static_preventive 时副标题标注 |
| `candidates[].nature` | enum? | `preventive` / `trusted` / `index` / `exploratory`（仅 static_preventive 必填；runtime 留空） | static_preventive 副标题"静态预防 · {nature}" |
| `candidates[].fix.reason` | string | 修改原因（融合命中细节 + 规则依据，一句话） | "修改原因"行 |
| `candidates[].fix.code_blocks[].title` | string | 修复要点 | 子标题 |
| `candidates[].fix.code_blocks[].language` | string | `cpp` / `ascendc` | 围栏标记 |
| `candidates[].fix.code_blocks[].before` | string | 违规代码（多行，保留缩进） | 双代码块之上 |
| `candidates[].fix.code_blocks[].after` | string | 修复代码（多行，保留缩进） | 双代码块之下 |

## 渲染规则（JSON → MD）

1. 候选顺序 = `candidates[]` 数组顺序。
2. 每个 `code_blocks[]` 元素渲染为一对 `before/after` 代码块，标题取 `title`，围栏语言取 `language`。
3. Markdown 报告始终保留标题和环境信息；`candidates[]` 为空时，在环境信息后输出"当前证据未形成已知异常候选；这不代表源码已验证安全。"，不生成候选章节。只完成 brief 时不得声称已扫描。
4. `category == "static_preventive"` 时，候选标题下追加副标题"*(静态预防 · {nature})*"。

## 不变量（生成后自检）

1. 非空 `code_blocks[].after` 必须是在目标工程上下文中可编译的真实代码；`before` 须对应当前源码，不能直接拿历史种子替代。未验证编译时不得声称已编译通过；不能确定安全修复时留空并在 reason 说明。
2. `code_blocks[].before`/`after` 的 `\n` 保留真实缩进。
3. `env` 四子段（`hardware`/`cann`/`skill`/`runtime`）任一缺失时，对应位置填 `"unknown"`，不得省略键。
4. `category == "static_preventive"` 时 `nature` 必填；`category == "runtime"` 时 `nature` 留空。
5. 信任型/索引型/探索型（`nature ∈ {trusted, index, exploratory}`）的 `code_blocks` 为 `[]`。brief、证据不足、无保持算子语义的安全修复时，runtime/预防型也使用 `[]`，说明尚未修改；只有已确认且能给出安全修复时才要求提供代码块。
6. `fix.reason` 明确区分已确认根因、待验证候选、防御建议和实际修改；OOB 兜底不能渲染为目标异常已验证。性质映射为：预防型→preventive、信任型→trusted、索引型→index、探索型→exploratory。
