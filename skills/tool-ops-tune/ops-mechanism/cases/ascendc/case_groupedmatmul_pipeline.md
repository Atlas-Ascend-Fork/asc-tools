# 案例卡：GroupedMatmul 流水线气泡三连优化（AIC:AIV 配比 → workspace 份数 → DoubleBuffer）

- **算子类别**: matmul / grouped matmul
- **DSL**: Ascend C
- **类型**: 非 PR（官方递进教程，cann-samples 2_Performance story 系列之一）
- **来源可信度**: C（官方指南/sample，可运行代码）
- **来源链接**: gitcode.com/cann/cann-samples 2_Performance story（**验证日期: 2026-10**）
- **瓶颈证据**: 多单元 30-50% 占比、无明显主导（流水气泡形态；对应本体系 L1_LATENCY_SYNC_SUSPECT / 串行指纹）
- **优化机制**: ① AIC:AIV 启动比例 1:1→1:2（Cube/Vector 耗时不匹配） ② workspace 份数 2→4（减少互等） ③ DoubleBuffer（锦上添花）
- **原文性能数字**:

| 阶段 | 耗时 | 增量收益 |
|------|------|---------|
| 基线 | 218.1 us | — |
| AIC:AIV=1:2 | 154.2 us | 29.3% |
| workspace 2→4 | 131.8 us | 14.5% |
| DoubleBuffer | 128.1 us | 2.8% |
| **合计** | **128.1 us** | **41.3%** |

- **适用范围与警示**: ⚠ 数字只代表来源环境；三步是**机制优先级**证据（配比 > workspace > DB），
  非每步收益承诺；混合绑核 UB 预算须重算。
- **对应机制卡**: pipeline.md
- **对应判定规则**: L1_LATENCY_SYNC_SUSPECT / L1_SERIAL_NO_OVERLAP
