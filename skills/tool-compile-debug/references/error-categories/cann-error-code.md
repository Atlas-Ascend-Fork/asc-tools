# cann_error_code — CANN 错误码（编译期分流）

## 根因

日志中出现 CANN 专有错误码（ACL_ERROR_* / RT_* / 内部错误码），但没有源码行号。
这类码覆盖编译期与运行期两个域，需先分流再处置。

## 错误码结构

CANN 错误码按前缀分族（完整映射见随包引擎 `scripts/engine/harness/knowledge/cann_error_codes.yaml`，337 条）：

| 前缀/值域 | 域 | 典型场景 | 处置 |
|---|---|---|---|
| `EI` / 内部错误 | 编译期 | bisheng/ccec 内部错误 | 本页 + [ICE 类别页](compiler-internal.md) |
| `UE0012` 系 / 参数错误 | 编译期 | 传参不合法 | 本页 + 相关类别页 |
| `ACL_ERROR_*`（161xxx） | **运行期** | 环境配置/Tiling/Kernel 查找 | 核实际失败阶段；纯运行故障交回调用方 |
| `ACL_ERROR_*`（361xxx） | **运行期** | Runtime 错误 | 同上 |
| `ACL_ERROR_*`（561xxx） | **运行期** | 内部错误 | 同上 |
| `RT_*` | **运行期** | Runtime 层 | 同上 |

## 分流线

```
日志中只有一个 CANN 错误码，无源码行
    │
    ├─ 有值域信息（如 161xxx）？
    │   ├─ 是 → 按上表核失败阶段（纯运行故障属于范围外）
    │   └─ 否 → 在 cann_error_codes.yaml 按值检索
    │
    ├─ 检索命中 fix_hints 族？
    │   ├─ 是 → 按 hint 的 suggested_actions 处置
    │   └─ 否 → 保留 unknown，补完整日志后重新诊断
    │
    └─ 日志里同时有源码行级报错？
        ├─ 是 → 首错以源码行为准，错误码作为辅助证据
        └─ 否 → 走本页分流
```

## 典型报错

```
EI0001: internal error
UE9000: invalid parameter
ACL_ERROR_RT_ ...
```

## 修法

1. 确认日志中是否有源码行级报错（file:line: error:）。有则以源码行为首错，错误码作为辅助证据；无则走本页分流。
2. 在 `scripts/engine/harness/knowledge/cann_error_codes.yaml` 按值或名称检索。命中则读关联的 fix_hint（`scripts/engine/harness/knowledge/fix_hints.yaml`）。
3. 结合命令和完整日志核失败阶段：构建链中的失败继续查当前阻塞；纯运行故障交回调用方，不只凭错误码前缀判定编译成功或失败。
4. 完全不认识的码：保留 unknown，让用户复跑构建获取完整日志（含源码行），再重新诊断。

> **边界**：本包处理编译失败；纯运行故障不进入编译修复闭环。编译补丁的必要验证出现运行失败时，保留失败事实并核与补丁的关系，不把编译通过当成运行成功。

## 说明

分类匹配规则与建议动作由随包引擎生成（`scripts/engine/harness/knowledge/error_signatures.yaml` 的 `cann_error_code` 规则族 + `cann_error_codes.yaml` 的 337 条码表）。错误码族的人话修复步骤见 `scripts/engine/harness/knowledge/fix_hints.yaml`（13 个族）。
