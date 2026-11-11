# package_error — 打包阶段错误

## 根因

编译和链接都过了，但**打包/部署阶段**出问题：产物里缺 kernel metadata、下游 `.json` 没生成、打包步骤失败。算子要被框架加载，需要打包阶段产出完整的 kernel 元信息和算子描述 json;缺了就装不上、调不到。

## 典型报错（真实日志样貌）

```
msobjdump: ... no ... kernel
kernel metadata missing
package failed
xxx.json ... not generated
failed to generate ... json
```
匹配规则:`msobjdump:.*no .*kernel|kernel metadata missing|package failed`(kernel 元信息缺失） + `\.json.*not generated|not generated.*\.json|failed to generate.*json`(json 未生成） → category `package_error`,phase `package`。

## 修法

1. **kernel metadata 缺失**:确认 kernel 真的编进了产物。用 `msobjdump` 检查产物里有没有 kernel 段;
若没有，往回查 kernel 编译是否实际产出（可能 kernel 编译被静默跳过 → 见 [kernel-compile-error.md](kernel-compile-error.md))。
2. **入口符号 mismatch**:打包期报入口符号对不上 → 核 host 期望的入口名与 kernel 导出名（同 link_error 的入口漂移）。
3. **json 未生成**:下游 json 由生成步骤产出，检查 msopgen/生成步骤是否执行、`$ASCEND_OPP_PATH` 是否指向构建产物。
stale/缺失生成产物的处理见 [dependency-missing.md](dependency-missing.md)。

## 说明

匹配规则见随包引擎 `scripts/engine/harness/knowledge/error_signatures.yaml`（`package_artifact_missing` / `json_not_generated`）； msobjdump 探针由随包引擎的探针模块提供。
