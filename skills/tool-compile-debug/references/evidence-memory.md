# 可恢复证据状态

在复杂任务跨轮、交接或多次修改容易遗失反证时使用。只保存可核对的观察、决定及引用，不保存内部推理过程；简单题继续用日志、diff和验证收据即可。

宿主先写一个小JSON事件，然后运行：

```bash
python3 "/实际技能目录/scripts/run.py" memory \
  --project "/实际工程" --event "/会话/事件.json" --out "/会话/state-1.json"
python3 "/实际技能目录/scripts/run.py" memory \
  --project "/实际工程" --state "/会话/state-1.json" \
  --event "/会话/后续事件.json" --out "/会话/state-2.json"
```

不传`--event`只复核已记录文件与依赖；输出仍是新快照，不覆盖历史。必要的外部日志/SDK文件用`--allow-root`显式列出允许路径，每次复核都需提供；可传文件本身以收窄范围。只hash指定的文件，最多64个、合计16MiB，不扫描工程。较大日志使用已有解析产物及其原文位置，不把大工程全部塞入状态。预算不足返回错误，不悄悄丢掉事实。

最小事件示例（内容和文件需替换成实际观察）：

```json
{
  "constraints": ["保留请求的target和实现路径"],
  "context": {"sdk": "9.0.0", "target": "Ascend910B1", "configuration": "实际配置身份", "recipe": "实际原配方"},
  "facts": [
    {"id": "call", "claim": "当前报错调用位置", "files": ["src/kernel.asc"],
     "roles": ["failure_unit", "instantiation"]},
    {"id": "declaration", "claim": "已读取的当前声明及约束",
     "files": ["include/contract.h"], "context_keys": ["sdk", "configuration"],
     "roles": ["declaration"], "origin": "host_observation"},
    {"id": "mismatch", "claim": "调用与声明之间的已观察错配",
     "depends_on": ["call", "declaration"], "origin": "inference"}
  ],
  "hypotheses": [
    {"id": "type", "claim": "需核实的类型解释", "depends_on": ["mismatch"],
     "support": ["call"], "refute": [], "next_question": "实际实例化类型是否符合声明？"}
  ],
  "obligations": [
    {"id": "original-target", "claim": "沿原配方编译受影响目标",
     "depends_on": ["mismatch"], "outcome": "pending"}
  ]
}
```

`context`一旦提供是**本次完整值替换**，漏掉的维度视为未知；工具不能自动发现SDK或命令变化。目标、SDK、配置、配方改变时必须刷新它。`context_keys`只列该事实实际依赖的维度。文件hash只能证明内容身份，不能证明claim正确；`current`也只指绑定尚有效。补记的hash不证明修改前状态，不能用此状态冒充verify的失败前态。

事实改动只使直接/传递依赖失效；无关文件不影响既有事实。同ID的证据、上下文或陈述改变时，旧判断失效，需重新核实后显式记录判断。仅重新登记同一内容、改变记录时间，不失效原判断，也不算新证据来重置重复提醒。已建立的任务约束不能通过事件放宽，任务确实改变时另建状态并说明用户的新要求。

机制查询：

```bash
python3 "/实际技能目录/scripts/run.py" inspect \
  --project "/实际工程" --question knowledge --mechanism template_visibility \
  --memory "/会话/state-2.json" --out "/会话/questions.json"
```

六类机制：`build_route`、`template_visibility`、`header_origin`、`generated_contract`、`version_target`、`link_closure`。问题返回所需`evidence_roles`及当前引用。`roles`由宿主按已读取证据填写；`host_evidence_available`不证明问题已经回答。缺口、探针、各结果的下一动作和最终验证义务在同一输出中，宿主选择是否执行。

一次CANNBOT返回可记入`collaborations`：

```json
{"collaborations": [{
  "id": "api-consult", "skill": "ascendc-docs-search",
  "claim": "本次问题的回答与采用依据", "depends_on": ["declaration"],
  "stage": "evidence_returned", "applicability": "applicable", "disposition": "adopted"
}]}
```

`stage`区分`discovered/loaded/executed/evidence_returned`；适用性为`unknown/applicable/conflict/inapplicable`；决定为`candidate/adopted/rejected`。采纳要求证据已返回、宿主核适用、依赖仍有效。它不自动调用或认证CANNBOT；记录实际调用材料及本地声明，不能把技能加载当修法依据。

重复无效路径可记录`attempt`，字段为`question/action/outcome/facts`。`outcome`取`progress/no_progress/infra_failure/blocked`，`facts`列相关事实ID；同问题、动作和证据再次`no_progress`时输出提醒。提醒不自动停任务，也不增加重试；有新证据或新阻塞仍可继续。任务时间、token与工具预算由宿主/实验调度器管理，memory不在后台运行。

义务的`outcome`取`pending/host_reported_pass/host_reported_fail/blocked`；工具不把宿主陈述升级为通过。最终验收仍引用原配方的验证报告和实际diff，编译、契约和必要行为验证分别交付。
