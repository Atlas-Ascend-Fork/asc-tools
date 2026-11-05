# ecc_ras — 硬件物理失效（ECC / parity / CRC）索引

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | ecc_ras |
| 异常类型 | 硬件物理失效（多 bit ECC / parity / CRC / 1bit 超水线） |
| 错误码 | 无（硬件 RAS 上报，应用层不可见） |
| 类别 | static_preventive |
| 性质 | 索引型 |
| 覆盖条目 | A-2.1 多 bit ECC（29）+ A-2.2 parity/CRC/1bit（6）= 35 条 |
| 检测时机 | 运行时（硬件 RAS / 驱动层） |
| 报错模式 | 无（应用层不可见，仅 RAS 告警） |

## EventID 索引表

> 物理内存 bit 翻转 / parity / CRC 失败导致，仅能通过硬件 RAS（Reliability/Availability/Serviceability）机制检测，**无任何 AscendC 代码注入路径**。

| EventID | 硬件单元 | 文档行 | 错误特性 | 处理方式 |
|---------|---------|--------|---------|---------|
| 0x80C98002 | CUBE | L5 | 读 MX buffer 时多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | CUBE | L10 | 读 L0C 时 2bit 不可纠正 ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | CUBE | L11 | 读 L0B 时 2bit 不可纠正 ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | CUBE | L12 | 读 L0A 时 2bit 不可纠正 ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | MTE | L23 | ROB MEM 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | MTE | L24 | UB MEM 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | MTE | L25 | tagmgr buffer 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | MTE | L26 | STB MEM 多 bit ECC（AIC） | RAS 告警 + 页隔离 |
| 0x80C98008 | MTE | L35 | L1 2bit ECC | RAS 告警 + 页隔离 |
| 0x80C98002 | MTE | L36 | NDDMA request buffer 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98002 | MTE | L37 | NDDMA cache 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | MTE | L58 | L1 MEM 2bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | FIXP | L75 | L0C ECC multi error | RAS 告警 + 页隔离 |
| 0x80C98002 | FIXP | L84 | STB multi-bit ECC | RAS 告警 + 页隔离 |
| 0x80C98002 | SU | L99 | SS buffer 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | SU | L104 | Parameter Buffer ECC 2bit | RAS 告警 + 页隔离 |
| 0x80C98008 | SU | L111 | dcache tag-ram ECC 2bit | RAS 告警 + 页隔离 |
| 0x80C98008 | SU | L112 | dcache data-ram ECC 2bit | RAS 告警 + 页隔离 |
| 0x80C98008 | SU | L116 | UB ECC 2bit | RAS 告警 + 页隔离 |
| — | VEC | L128 | context switch 访问 sram 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | VEC | L130 | UB uncorrectable ECC（SFU/TSU/VF） | RAS 告警 + 页隔离 |
| 0x80C98008 | VEC | L137 | parameter buffer 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | VEC | L150 | DVG Stack 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | VEC | L151 | MRF 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | VEC | L152 | Vthread ID 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | VEC | L153 | dirty MEM 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | VEC | L154 | DCACHE TAG 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | VEC | L155 | MROB 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98008 | VEC | L156 | BHU 多 bit ECC | RAS 告警 + 页隔离 |
| 0x80C98005 | MTE | L34 | CFG REG parity error | RAS 告警 |
| 0x80C98006 | SU | L97 | warn as exception（15 次 1bit ECC 或 IFU multi-hit） | 累计后上报 exception |
| 0x80C98006 | SU | L100 | 1bit ECC 超水线 | 累计 15 次后上报 |
| 0x80C98008 | SU | L103 | MTE CRC 校验错误 | RAS 告警 |
| 0x80C98002 | SU | L108 | SU FIFO parity bit 比对出错 | RAS 告警 |
| 0x80C98005 | VEC | L127 | nmanager parity error | RAS 告警 |

## 应用层建议

1. **内存隔离**：RAS 驱动检测到 ECC 故障页后执行页隔离（page retirement）
2. **降频运行**：高温场景下降低 AI Core 频率以减少 ECC 错误率
3. **数据冗余**：关键计算使用双 buffer 交叉校验（应用层）
4. **定期健康检查**：通过 `npu-smi info` 监控 ECC 计数趋势

## 检测执行指引

- **无规则匹配**：索引性文档，不扫源码
- diagnose Step 0 不主动输出；仅在 keyword 命中（如 "ECC" / "RAS" / "parity" / "CRC"）时返回 EventID 索引
