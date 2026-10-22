# 故障分流表（采集 / 编译 / 运行 / 对比）

> 三列结构：故障现象 → 判定/根因 → 动作。条目来自一线调优实测沉淀，按调优域组织。

## 采集类

| 现象 | 根因 | 动作 |
|------|------|------|
| `--kernel-name` 采集为空但应用正常 | C++ mangled 匿名命名空间符号（`_ZN12_GLOBAL__N_1...`） | 去掉过滤全量采集，从 OpBasicInfo 抄真实 kernel 名重采 |
| fork 多进程只采到每进程第一个 kernel | msopprof 对 fork 子进程不完整 | 标 partial；改 SHMEMI_PROF 打点或完整 msprof（task-based） |
| 采集日志 libprofapi ERROR 但 CSV 正常 | 工具链噪声（CANN 9.1 实测） | **以产出检查为准**，不凭 ERROR 行判失败 |
| msprof op 拉起 MC² 程序数据可疑/挂 | 对 fork 行为未定义 | 强制回退 msprof --aic-mode=task-based |
| 输出目录直接报错 | 权限/软链检查 | 确认目录可写、无跨设备软链 |
| PipeUtilization 只有 N 个 block 行 | 数据量小或切核不足 | 正常——这正是 L2_UNDERUTILIZATION 的判定输入 |
| rankNum > 空闲设备数 | 环境 BLOCKED | 判定阶段即拦截（不进入采集）；换卡/减 rank |

## 编译类

| 现象 | 根因 | 动作 |
|------|------|------|
| `GLIBCXX_3.4.32 not found` | miniconda libstdc++ 过旧（TBE 编译依赖） | 换系统库软链 |
| ffn 系列缺 `aic-ascend950-ops-info.ini` | 复合算子 6 子算子缺 A5 配置 | BLOCKED 等上游，如实标注 |
| MicroAPI / Reg API 编译报错 | 命名空间/签名/路径差异 | **修复顺序**：命名空间别名 → 签名适配 → include → 降级留痕 |
| 仿真拉起 signal 6 / Bad address | 仿真需要仿真兼容构建产物 | 走 cannsim 路径的兼容构建说明（`$ASCEND_HOME_PATH/tools/` 或昇腾文档站「cannsim」章节） |

## 运行类

| 现象 | 根因 | 动作 |
|------|------|------|
| tiling 改动后 device fault | 容量/深度联动假设被破坏 | ①回滚确认基线可复跑 ②-g 重编 + msSanitizer 定位 ③缩小改动粒度（baseK 128→192 非 256）④核对联动字段 |
| kernel 挂起 | 同步协议改动死锁等 | **挂起即回滚**，不在 hang 状态上调性能；不可拆分原子对先最小二分 |
| 精度失败（任一 case） | 机制引入语义变更 | 转调试域；该轮性能数字不得进报告 |

## 对比类

| 现象 | 根因 | 动作 |
|------|------|------|
| delta_compare exit 2（口径不一致） | HARD_KEYS 漂移（设备/口径/shape…） | 重建基线——跨口径比较无意义，禁止绕过 |
| delta_compare exit 2（精度非 pass） | 第三道门禁 | 处置精度，禁止生成性能结论 |
| 短 kernel event wall 无收益但 profiler 有收益 | 计时口径问题 | 多 launch 单 event 或 device duration 辅助口径并注明 |
| 收益在 ±5% 内反复横跳 | 噪声带 | repeat 翻倍复测（同步重采基线与优化侧、新目录重建快照——repeat 是 HARD 口径字段）；仍带内按无稳定收益 |
| 首轮极慢（差可达 25%） | 冷跑（初始化/缓存预热/DVFS） | 固定 warmup 剔除；稳态 ≥3 取中位 |
| 环境指纹变化后旧基线全失效 | 跨环境比较禁令 | 正常行为：重建基线（这正是指纹存在的意义） |
