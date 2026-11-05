# cross_references.md — 跨异常关联

异常之间的跨类别关联关系：参数共享、错误传播链、症状相似组、共享检测规则。

> **路径约定**：本文引用的所有异常 ID 对应 `references/detectors/runtime/{category}/{ID}.md`，category 见 `seed_mapping.md`。

## 参数共享

不同异常共享的参数及其约束关系。

| 参数 | 涉及异常 | 约束 |
|------|---------|------|
| blockLen | [datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow] | blockLen > 0 且 blockLen ≤ buffer_size/32 |
| blockCount | [datacopy_burst_num_illegal, mte_read_ub_overflow] | blockCount > 0 且总跨度不超出目标内存区域 |
| dstStride | [datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow] | 按实际 API/重载把块长、gap、次数和起始偏移统一成字节后检查目标范围 |
| srcStride | [fixpipe_l1_overflow, fixpipe_srcstride_overflow] | 按 Fixpipe 源布局计算实际读取区间并与源分配比较；不能从字段名推导为 L1 |
| padding | [padding_misconfig] | padding 后 fmap 尺寸 >= filter 尺寸 |

## 错误传播链

异常之间的因果关系——一个异常触发后可能级联触发另一个异常。

| 传播路径 | 描述 |
|---------|------|
| first_window_overflow → ub_addr_misalign | 候选传播方向，须追溯实际地址才能确认；不能将两种症状直接视为已证实因果 |

## 症状相似

按症状维度聚合具有相似表现的异常，用于无错误码时的候选集收窄。

| 症状 | 候选异常 | 区分要点 |
|------|---------|---------|
| kernel hangs | flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig | flag_mismatch: flag 不配对（SyncBlockWait 无配对 SyncBlockArrive）; alloc_free_mismatch: AllocTensor 后无 FreeTensor; enque_deque_mismatch: EnQue 后无 DeQue; padding_misconfig: 有错误码 41 但实际挂起，LoadData3D padding 配置导致 fmap < filter |
| UB overflow | vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, datacopypad_ub_overflow | vec_ub_overflow: Vector 操作跨度超出 UB 边界; mte_write_ub_overflow: MTE 写 UB 地址溢出; mte_read_ub_overflow: MTE 读 UB 地址溢出; datacopypad_ub_overflow: DataCopyPad 参数导致 UB 写越界 |
| GM/L2 overflow | gm_overflow, l2_write_overflow | gm_overflow: GM 地址越界; l2_write_overflow: L2 写越界 |
| FixPipe overflow | fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow | fixpipe_gm_overflow: FixPipe 写 GM 溢出（错误码 141）; fixpipe_cfg_illegal: FixPipe 配置非法; fixpipe_l1_overflow: FixPipe 读 L1 越界; atomic_fixpipe_overflow: FixPipe AtomicAdd GM 溢出（错误码 141，与 fixpipe_gm_overflow 碰撞） |

## 共享检测规则

由交叉校验合并的共享规则集中存储于本文件，以供校验和追溯；diagnose 需要时与本文件比对补强。每条共享规则给出【条件】【API】【约束】【匹配模式】四标记（与 detector 检测规则同口径）。

| 共享规则 ID | 规则描述（四标记摘要） | 适用的多个异常 ID |
|-------------|----------------------|------------------|
| SHARED-BOUNDARY-01 | **【条件】**内存偏移 + 写/读长度可能超出目标缓冲区容量 **【API】**`SetGlobalBuffer` / `DataCopy` / 指针算术（`base + offset`） **【约束】**`offset + length <= BUFFER_SIZE`（BUFFER ∈ {GM, L2, UB, L1}） **【匹配模式】**值域判断 | gm_overflow, l2_write_overflow, ub_capacity_overflow, vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, datacopypad_ub_overflow |
| SHARED-STRIDE-01 | **【条件】**按实际 API 计算的块跨度加起始偏移可能超出分配 **【API】**`DataCopy` / `DataCopyPad` / FixPipe stride 参数 **【约束】**按 `references/constraints/semantic_contract.md` 统一单位；普通 DataCopy 使用 `offset + (count*len+(count-1)*gap)*32 <= alloc`，其他重载单独确认 **【匹配模式】**值域判断 | datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow, fixpipe_l1_overflow, fixpipe_srcstride_overflow |
| SHARED-BURST-01 | **【条件】**DataCopy 的 blockLen/blockCount 取值非法（≤0 或超过缓冲区容量折算） **【API】**`DataCopy`（blockLen / blockCount 参数） **【约束】**`blockLen > 0 && blockLen ≤ buffer_size/32` 且 `blockCount > 0` **【匹配模式】**值域判断 | datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow |
| SHARED-PAIR-ALLOC-FREE | **【条件】**同一作用域内 `AllocTensor` 与 `FreeTensor` 调用次数不一致 **【API】**`AllocTensor` / `FreeTensor` **【约束】**`∀ Queue Q: COUNT(Q.AllocTensor) == COUNT(Q.FreeTensor)` **【匹配模式】**API 调用模式 | alloc_free_mismatch |
| SHARED-PAIR-ENQUE-DEQUE | **【条件】**同一队列 `EnQue` 与 `DeQue` 调用次数不一致或顺序颠倒 **【API】**`EnQue` / `DeQue` **【约束】**`∀ Queue Q: COUNT(Q.EnQue) == COUNT(Q.DeQue)` 且 DeQue 晚于 EnQue **【匹配模式】**API 调用模式 | enque_deque_mismatch |
| SHARED-PAIR-FLAG | **【条件】**`SetFlag` / `WaitFlag`（或 `SyncBlockArrive` / `SyncBlockWait`）不配对 **【API】**`SetFlag` / `WaitFlag` / `SyncBlockArrive` / `SyncBlockWait` **【约束】**每一 WaitFlag 存在先行的 SetFlag，且 flag 类型一致 **【匹配模式】**API 调用模式 | flag_mismatch |
| SHARED-ALIGN-01 | **【条件】**地址或 tiling 参数未满足对齐要求（32B / 块对齐） **【API】**`SetGlobalBuffer` / `AllocTensor` / tiling 计算 **【约束】**`addr % alignment == 0`（UB 通常 32B 对齐） **【匹配模式】**存在性检查 | ub_addr_misalign, tiling_ub_misalign |
| SHARED-PADDING-01 | **【条件】**padding 后 fmap 尺寸小于 filter 尺寸 / LoadData3D padding 配置非法 **【API】**`LoadData3D`（padding 参数） **【约束】**`fmap_after_padding >= filter_size` **【匹配模式】**值域判断 | padding_misconfig, fmap_size_illegal |

diagnose 可按需读取本节，依据“适用的多个异常 ID”列选取共享规则，与候选 detector 的证据比对补强。共享规则仅用于补充诊断解释，可在 `fix.reason` 中注明依据，不额外计入 `rule_hit_rate` 的分子或分母；计分仍以 detector 自身规则及 `SKILL.md` Step 2 的口径为准。与自身规则重叠的违规点仅按自身规则计分；未读取本节不影响常规规则匹配与排序。
