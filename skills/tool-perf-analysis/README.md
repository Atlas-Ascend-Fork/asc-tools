# tool-perf-analysis — NPU 算子性能报告解读 SKILL

读取 NPU 算子性能采集产物，产出含**基本信息 / 问题 / 证据 / 优化候选方案**的专业
性能诊断报告（`report.md` + `report.html` 双格式）。

- **输入 A（Ascend 950 / dav-3510）**：npu-compute 采集的 `.npu-rep` 报告
- **输入 B（非 950）**：msopprof 采集的 `PROF_*` 目录（`--soc a2|a3|a5` 必填）
- **判定引擎**：外部引用 [tool-ops-tune](https://gitcode.com/cann/asc-tools)
  （skills/tool-ops-tune）——瓶颈判定出自 `ops-verdict/scripts/verdict.py` 规则引擎
  （L3→L2→L1→L0 分层瀑布 + 规则-指标绑定契约 + 反证 + 置信度），优化候选方案出自
  `ops-mechanism` 机制卡（八要素）。本技能只做取数、编排、解释与呈现，**不做判定**。
- **耗时**：脚本链实测 <1s（30s 预算的绝大部分留给 LLM 解读层）。

## 快速使用

```bash
# 950 报告（SoC 自动识别）
python3 scripts/diagnose.py --rep <report.npu-rep> -o <outdir>

# msopprof 采集目录
python3 scripts/diagnose.py --prof <PROF_dir> --soc a2 -o <outdir>

# 回归（4 组用例；.npu-rep fixture 本地获取，缺省自动 SKIP）
python3 evals/run_evals.py
```

产物：`facts.json`（事实）+ `verdict.json`（判定书）+ `diagnose_bundle.json`
（渲染数据源）+ `report.md` / `report.html`（报告，同一数据源双格式）。

## 执行链

```
.npu-rep / PROF_*
   │ scripts/unpack_npu_rep.py（解包，npu-rep 源）
   ▼
scripts/extract_facts.py ──→ facts.json（取数 + NA 剔除 + ratio 归一 + soc-spec 峰值换算）
   │                         + verdict --scalar 串（facts_bridge 同款标量契约）
   ▼
tool-ops-tune ops-verdict/scripts/verdict.py ──→ verdict.json（判定书，唯一判定出口）
   │ findings[].mechanism_refs
   ▼
tool-ops-tune ops-mechanism/references/optimize/*.md（机制卡八要素，正文内嵌报告）
   ▼
scripts/render_report.py ──→ report.md + report.html（五节固定框架）
```

## 设计要点

- **判定进脚本、解释进 LLM**（与 tool-ops-tune D1 原则一致）：瓶颈判定唯一出口是
  规则引擎的 `verdict.json`，本技能不产出规则库之外的判定；无证据时如实
  INSUFFICIENT_DATA，不冒充结论。
- **npu-compute `.npu-rep` 独有能力**：`summary.db` PcSampling 表 → `top_stall_pct`
  （L0 指令级判定路径）；PipeTrace 采样汇总；嵌套子报告递归解包。
- **msopprof 源差异处理**：`OpBasicInfo.csv` 承担算子信息角色；无 HardwareInfo 时
  强制 `--soc`（fail loud，防 A2/A5 标尺错位）；SIMT 内核 pipe 占比求和可 >100%
  （并行重叠口径，与 npu-compute 串行口径区分解读）。
- **口径纪律**：NA 不当 0；ratio 0~1/0~100 双口径归一；active_bw 活跃期口径注记；
  single-shot 非稳态标注；降频失真标注；多 kernel 必须 `--kernel-name` 过滤；
  physical_cores 按 HardwareInfo 实测 > skus 匹配 > reference 顺序解析。
- **报告五节固定框架**：基本信息 / 瓶颈判定（问题）/ 证据（关键指标）/ 优化候选
  方案（机制路由）/ 数据缺口与补采建议；收益区间一律标注「典型收益，非本算子
  预测」；反证如实呈现。

## 安装

放入 Claude Code 技能目录（`~/.claude/skills/tool-perf-analysis/` 或项目级
`.claude/skills/`）。tool-ops-tune 自动探测序：`--ops-tune` 参数 >
`OPS_TUNE_HOME` 环境变量 > 套件形态（`~/.claude/skills/tool-ops-tune/`、同级
`asc-tools*/skills/tool-ops-tune`）> 平铺形态（`ops-verdict` + `ops-mechanism`
作为兄弟技能目录）。
