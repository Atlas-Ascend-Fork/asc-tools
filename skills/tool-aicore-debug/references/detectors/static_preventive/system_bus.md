# system_bus — 总线/系统级错误索引

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | system_bus |
| 异常类型 | 总线/系统级（单核不可达） |
| 错误码 | 无（应用层不可见，驱动/固件层处理） |
| 类别 | static_preventive |
| 性质 | 索引型 |
| 覆盖条目 | B 类：L66 biu_unsplit / L67 SMMU / L91 link bus timeout / L138 VEC BIF（4 条） |
| 检测时机 | 运行时（总线/驱动层） |
| 报错模式 | 无（应用层不可见） |

## EventID 索引表

| 文档行 | EventID | 硬件单元 | 错误特性 | 处理方式 |
|--------|---------|---------|---------|---------|
| L66 (BIU) | 0x80C98009 | BIU | biu_unsplit_err（tag_id overflow / FIFO 溢出） | 运行时监控 BIU STATUS 寄存器；驱动层处理 |
| L67 (BIU) | NA | BIU/SMMU | DFx error（SMMU） | SMMU 驱动层 BRESP/RRESP 监控；系统级配置 |
| L91 (SC) | 0x80C98009 | SC | link bus timeout | 运行时总线 timeout 计数器监控；需总线拥塞场景 |
| L138 (VEC) | 0x80C98002 | VEC/BIF | VEC BIF 返回数据错误 | 多卡场景下 RAS 网络链接超时告警；需跨 device 访问 |

## 应用层建议

1. **L66**：运行时监控 BIU STATUS 寄存器；驱动层 tag_id / FIFO 溢出检测
2. **L67**：SMMU 配置属系统级，需 SMMU 驱动层注入处理
3. **L91**：运行时总线 timeout 计数器监控（需多卡/压力环境）
4. **L138**：多卡场景下 RAS 网络链接超时告警

> **修复建议**：均属系统级问题，需驱动/固件层处理；应用层无法修复。

## 关联异常

- **fixpipe_biu_error** / **dcache_biu_response** / **mte_biu_rdwr_resp**（runtime）：BIU 响应类异常的运行时表现
- **atomic_dirty_cacheline**（runtime）：cache coherence 类异常

## 检测执行指引

- **无规则匹配**：索引性文档，不扫源码
- diagnose Step 0：不主动输出；仅在 keyword 命中（如 "BIU" / "SMMU" / "BIF" / "总线"）时返回 EventID 索引
- 优先级：仅查询时可见
