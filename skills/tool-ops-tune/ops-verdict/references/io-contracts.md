# 输入/输出数据契约（cases.csv / case_candidate / calibration_data）

> 三个此前无 schema 的数据契约（检视 F21）：输入侧 cases.csv 只有「shape/dtype/attrs」
> 一句话；输出侧 case_candidate.json / calibration_data.json 只在 Step 5 被点名要求
> 产出。E 级回灌与校准回路（27 年两个核心回路）的输入格式由此定型，与现有 7 个
> 数据契约同一写法（对齐 USAGE.md §5 契约表）。

## 1. cases.csv（输入：测试用例清单）

最小字段（带表头的一行一 case；多余列允许，逐 case 透传进报告）：

| 列 | 必需 | 类型 | 说明 |
|----|------|------|------|
| `case_id` | ✅ | string | 全局唯一（报告逐 case 追溯的锚点） |
| `shape` | ✅ | string | 如 `8448x4096`（多维用 x 分隔） |
| `dtype` | ✅ | string | `fp16` / `fp32` / `bf16` / … |
| `attrs` | — | string | 算子属性（如 `axis=-1,keepdim=true`；无属性留空） |
| `ref_impl` | — | string | 标杆调用描述（P0-1 标杆双路径：非空时 collect_cases.py 该 case 跑 system 变体；如 `aclnnGelu` / `torch.nn.functional.gelu`） |

```csv
case_id,shape,dtype,attrs
c01,8448x4096,fp16,
c02,4096x4096,fp16,
c03,1024x1024,bf16,axis=-1,keepdim=true
```

> >20 个 case 取前 20 并告知用户截断（A1 铁律）；截断必须发生在读入侧，
> 不得静默丢弃。

## 2. case_candidate.json（输出：E 级案例回灌条目）

Step 5 产出，回灌 ops-mechanism 案例库的候选（经评审后入库）：

| 字段 | 必需 | 说明 |
|------|------|------|
| `schema` | ✅ | `opstune.case-candidate/1.0` |
| `operator` | ✅ | 算子名 |
| `scene_fingerprint` | ✅ | 口径指纹（soc/shape/dtype/cann_version…，取自 scope.json） |
| `bound_rule` | ✅ | 命中的 rule_id（证据绑定） |
| `mechanism` | ✅ | 实施的机制名 |
| `speedup_measured` | ✅ | 实测加速比（comparison.md 的 KEEP 轮） |
| `speedup_est` | ✅ | 候选方案卡的预估值（校准对照） |
| `precision` | ✅ | `pass`（fail 的轮次禁止产出条目） |
| `rollback_happened` | ✅ | bool（是否有回滚；回滚原因写入 notes） |
| `notes` | — | 适用边界 / 警示（如「仅 fp16 大 shape 验证」） |
| `generated_at` | ✅ | ISO 8601 |

```json
{
  "schema": "opstune.case-candidate/1.0",
  "operator": "gelu",
  "scene_fingerprint": {"soc": "a5", "shape": "8448x4096", "dtype": "fp16", "cann_version": "8.3.0"},
  "bound_rule": "L1_VEC_BOUND",
  "mechanism": "ub-fusion",
  "speedup_measured": 1.53,
  "speedup_est": [1.1, 1.3],
  "precision": "pass",
  "rollback_happened": false,
  "notes": "cast-heavy 反证命中场景；仅 fp16 大 shape 验证",
  "generated_at": "2026-10-14T12:00:00"
}
```

## 3. calibration_data.json（输出：预估-实测校准对照）

Step 5 产出，27-2 预估-实测校准回路的输入：

| 字段 | 必需 | 说明 |
|------|------|------|
| `schema` | ✅ | `opstune.calibration/1.0` |
| `operator` | ✅ | 算子名 |
| `entries[]` | ✅ | 每轮一条：`mechanism` / `bound_rule` / `speedup_est`（区间）/ `speedup_measured` / `verdict`（KEEP/REVERT/噪声带内）/ `round` |
| `generated_at` | ✅ | ISO 8601 |

```json
{
  "schema": "opstune.calibration/1.0",
  "operator": "gelu",
  "entries": [
    {"round": 1, "mechanism": "ub-fusion", "bound_rule": "L1_VEC_BOUND",
     "speedup_est": [1.1, 1.3], "speedup_measured": 1.53, "verdict": "KEEP"},
    {"round": 2, "mechanism": "mte2-preload", "bound_rule": "L1_MTE2_BOUND",
     "speedup_est": [1.2, 2.0], "speedup_measured": 0.98, "verdict": "REVERT"}
  ],
  "generated_at": "2026-10-14T12:00:00"
}
```

## 纪律

- 三个契约的示例文件必须通过各自 schema 校验（jsonschema 或等价手工断言）；
- `speedup_measured` 只能来自 delta_compare 的 KEEP 轮（噪声带内/REVERT/精度 fail
  的数字禁止作为「实测收益」写入 case_candidate，但 REVERT 条目照常进 calibration
  ——负样本是校准的输入）。
