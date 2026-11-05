# load3d_illegal_stride — LoadData3D stride 非法（strideH=0/strideW=0）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | load3d_illegal_stride |
| 硬件事件编号 | 13 |
| 异常类型 | LoadData3D stride 非法（配置非法类异常，strideW=0/strideH=0） |
| 错误码 | errcode:TBD stride illegal 类（验证阶段回填） |
| 类别 | loaddata |
| 源文件 | error/main.asc:72（`uint8_t strideH = 0, strideW = 0;`） |
| 报错模式 | EZ9999（验证阶段补充 retCode/errcode 字段） |
| 内存区域 | L1/A2（LoadData3D fmA1→fmA2 搬运路径） |
| API层级 | TC-High |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（strideH=0,strideW=0→1,1）+ return 1→0 |
| behavior_note | LoadData3D strideW/strideH=0 使步长为零，触发 AICoreError.md L45 ILLEGAL_STRIDE |

## 触发条件

**违规模式**：完整 Cube 流水线（DataCopy→LoadData3D→Mmad→DataCopyCO1），`LoadData3DParamsV2<half>` 构造时 `strideH = 0; strideW = 0` 使步长为零，触发 AICoreError.md L45 ILLEGAL_STRIDE（"stride_w或者stride_h等于0"）。mStartPt=0、kStartPt=0 保持合法，唯一错误源是 stride 为零。

**安全模式**：`strideH = 1, strideW = 1` — 恢复合法步长值。

## 检测规则

### load3d_illegal_stride-RULE-01：LoadData3DParamsV2 strideW/strideH 为零

- 【条件】LoadData3DParamsV2 构造参数中 strideW 或 strideH 等于 0
- 【API】`LoadData3DParamsV2<half> ld3d(padList, H, W, l1_3d_size, k, m, kStartPt, mStartPt, strideW, strideH, Kw, Kh, dilationW, dilationH, ...);`
- 【约束】strideW/strideH 不允许为 0；strideH=0 或 strideW=0 → AICoreError.md L45 ILLEGAL_STRIDE
- 【匹配模式】值域判断

### load3d_illegal_stride-RULE-02：stride 常量注入（0→1）

- 【条件】strideH/strideW 通过 `uint8_t strideH = 0, strideW = 0` 构造，触发零步长
- 【API】`uint8_t strideH = 0, strideW = 0;`
- 【约束】stride=0 → LoadData3D 步长为零 → 指令配置非法
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 strideH/strideW 从 0 恢复为合法步长 1。

```cpp
// 修复前（error）
uint8_t strideH = 0, strideW = 0;

// 修复后（fixed）
uint8_t strideH = 1, strideW = 1;
```

**防御性编程建议**：

- LoadData3D 前 assert `strideW > 0 && strideH > 0`
- 封装 LoadData3D 参数校验 wrapper，拒绝 stride 零值

## 变体索引

| 变体 | strideH | strideW | 触发路径 |
|------|---------|---------|---------|
| 基准（本用例） | 0 | 0 | L45 ILLEGAL_STRIDE |
| 单 stride 变体 | 0 | 1 | L45 ILLEGAL_STRIDE |
| 合法变体 | 1 | 1 | 安全（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：stride 合法 | strideW > 0 且 strideH > 0 | fixed 版（1,1） |
| EC2：stride 非法 | strideW == 0 或 strideH == 0 | error 版（0,0） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| load3d_km_start_illegal (14) | 同族 LoadData3D 配置非法，mStartPt 路径 |
| load3d_km_step_illegal | 同族 LoadData3D 配置非法，kStartPt 路径 |
| load3d_chn_size_illegal | 同族 LoadData3D 配置非法，channelSize 路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01）、diff_pattern（RULE-02）
- **命中判定**：RULE-01 在 strideW 或 strideH 任一为 0 时命中，RULE-02 核对 0→1 常量差异；种子保持 mStartPt=0、kStartPt=0 合法。
  - 读取 error/main.asc 第 72 行 `uint8_t strideH = 0, strideW = 0` 确认 stride 零值。
  - 读取第 37-39 行 `LoadData3DParamsV2<half> ld3d(...)` + `LoadData(fmA2, fmA1d, ld3d)` 确认 stride 参数传入 LoadData3D。
  - 计算 strideW=0 → 触发 AICoreError.md L45 ILLEGAL_STRIDE。
  - error 版 stderr 应含 EZ9999（验证阶段补充 retCode/errcode）。
- **修复映射**：
  - load3d_illegal_stride-RULE-01 → 保证 `strideW > 0 && strideH > 0`。
  - load3d_illegal_stride-RULE-02 → 将零步长改为 fixed 第 72 行的 `strideH = 1, strideW = 1`。
