# unfriendly_error_code — 编译器拒绝非法选项/arch（措辞不友好）

## 根因

把一个**非法的值**传给了编译器/工具链（常见：不支持的 NPU arch、未指定 aicore arch、不存在的编译选项）, 编译器拒绝，但措辞不友好、且后面常跟一串崩溃后果(LLVM ERROR / Stack dump)掩盖真因。与 `environment` 的区别：那是变量**没配**;这是变量**配了但值非法**,在 kernel 编译期被拒。

## 典型报错（真实日志样貌）

```
Unsupported NPU architecture
no aicore arch specified
Cannot find option named '...'
```
匹配规则:`Unsupported NPU architecture|no aicore arch specified|Cannot find option named` → category `unfriendly_error_code`,phase `kernel_compile`。该规则**排在 internal_compiler_error 之前**, 让真错误先于其引发的 ICE artifact 被捕获。

## 修法

1. **校正 arch/SoC 选项**：核对构建配置 `SOC_VERSION` / `CMAKE_ASC_ARCHITECTURES` 以及实际编译命令中的 `--npu-arch` 选项，使其与用户要求的目标芯片一致。
SoC↔arch 码映射见 [`../knowledge/soc-arch-gating.md`](../knowledge/soc-arch-gating.md)。
2. **补 aicore arch**:`no aicore arch specified` → 在构建配置里显式指定目标 arch。
3. **改选项名**:`Cannot find option named` → 该编译选项在当前 bisheng 版本不存在，删掉或换当前版本的等价选项。
4. **忽略后续崩溃**:本类报错后面的 `LLVM ERROR`/`Stack dump` 是拒绝的后果，不是独立根因，修了选项即消。

## 说明

匹配规则见随包引擎 `scripts/engine/harness/knowledge/error_signatures.yaml`（`compiler_unsupported_arch_or_option`，置于 `internal_compiler_error` 之前匹配）；SoC 与架构代际事实见 `scripts/engine/harness/knowledge/soc_feature_matrix.yaml`。
