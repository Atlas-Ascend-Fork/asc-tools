# hccn_tool — RoCE/HCCS 链路与网络诊断

> 排查 NPU 间 RoCE/HCCS 链路、网卡、路由、带宽、丢包等"通信层以下"问题。通信异常排除 rank/算子侧后，用它验证底层链路是否健康。

## 工具定位

driver 自带网络/链路诊断工具（`/usr/local/Ascend/driver/tools/hccn_tool`）。

**设备编号**：先用当前环境的 `npu-smi info` 确认目标设备。Atlas A3 官方导读将 `-i` 定义为 `Phy-ID`；不要把 NPU 模组编号、Chip 列或 ACL 逻辑编号直接代入，也不要沿用另一台机器的数字。下面的 `PHY_ID` 表示已确认的目标芯片物理 ID。接口支持范围以当前驱动版本和工具帮助为准，工具调用串行执行。

## 调用命令

优先使用已有日志和查询结果。离线自检、压测、清计数和配置操作会影响设备或诊断历史，仅在当前任务确有需要且使用范围已授权时执行；下面的压测命令是按需选项，不是默认采集步骤。

```bash
PHY_ID="${PHY_ID:-0}"  # 替换为当前 npu-smi info 核实后的物理 Phy-ID
case "$PHY_ID" in
  ''|*[!0-9]*) echo "invalid PHY_ID: $PHY_ID" >&2; exit 1 ;;
esac

# 1. 链路状态（UP/DOWN）+ 网络健康
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -link -g          # → "link status: UP"
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -net_health -g     # → "net health status: Success"

# 2. 复现前后丢包/错包对比
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -stat -g           # 复现前保存计数
# 跑业务复现
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -stat -g           # 复现后与前一份计数比较增量

# 3. 链路历史抖动（flap 次数）+ 带宽
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -link_stat -g      # 历史 flap/抖动
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -bandwidth -g time 1000  # 实时 TX/RX 带宽

# 4. 跨机链路验证（路由/ARP/光模块/RoCE 压测）
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -route -g          # 路由表（核对 HCCL_IF_IP）
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -arp -g            # ARP 表（跨机是否学到对端 MAC）
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -optical -g        # 光功率/温度（跨机长链路）
/usr/local/Ascend/driver/tools/hccn_tool -i "$PHY_ID" -roce_test ib_read_bw   # RoCE 带宽压测
```

**关键子命令**：
| 子命令 | 用途 |
|--------|------|
| `-link -g` | eth 端口链路状态（UP/DOWN）+ down_fault_time |
| `-net_health -g` | 网络健康状态（Success/Failed） |
| `-stat -g` | 保存复现前后收发包统计，比较增量；不需要先清零共享计数器 |
| `-link_stat -g` | 链路历史统计（flap/抖动次数） |
| `-bandwidth -g [time <ms>]` | 实时 TX/RX 带宽 |
| `-eth_test -g offline` | eth 端口自检 |
| `-ip -g` / `-gateway -g` / `-route -g` | IP/网关/路由（核对 HCCL_IF_IP） |
| `-arp -g` | ARP 表（跨机直连 MAC 学习） |
| `-optical -g` | 光模块（光功率、温度） |
| `-roce_test ib_read_bw/ib_send_lat` | RoCE 性能压测（带宽/延迟） |
| `-version -g` | 固件/驱动版本 |

## 输出解读

| 输出字段 | 含义 |
|---------|------|
| `link status: UP/DOWN` + `down_fault_time` | 物理链路通断 |
| `net health status: Success/Failed` | 网络健康 |
| `rx_discards` / `tx_errors` / `rx_errors` 增量 | 丢包/错包（复现前后对比） |
| flap 次数（`-link_stat -g`） | 偶发断链记录 |
| 实时 TX/RX 带宽 | 带宽类问题定位 |

## 诊断决策

| hccn_tool 输出特征 | 指向 | 详见 |
|-------------------|------|------|
| `-link -g` 返回 DOWN 或 `-net_health -g` 返回 Failed | `anomaly_module = 链路`，物理链路/网络故障 → [communication-timeout](../symptoms/communication-timeout.md) | |
| 复现前后 `-stat -g` 丢包/错包增量显著 | 发现丢包/错包迹象；结合设备范围、复现时间和 rank 日志判断是否关联当前超时 | |
| `-link_stat -g` flap 次数高 | 偶发断链（驱动/固件/光模块稳定性） | |
| 本次链路 UP/Success 且无丢包增量 | 本次采样未发现链路异常；核对故障时间窗口及采样覆盖后，继续检查 rank 参与对等性 → [communication-timeout](../symptoms/communication-timeout.md) | |
| `-roce_test ib_read_bw` 带宽不达预期 | 链路带宽瓶颈，配合 [msprof](msprof.md) `--sys-io-profiling` | |
| `Get invalid device id.` | 核对当前工具的编号语义、可见设备及容器映射，不盲试其他机器的编号 | |

## 降级策略

- 工具不可用或无执行权限：`tool_evidence` 标注 `skipped: hccn_tool not accessible`，降级为 [hccl-test](hccl-test.md) 间接验证链路（hccl_test 失败则链路层可疑）
- 跨机无 root 时：用 [plog](plog.md) 的 `error cqe`/`socket connect fail` 关键词间接判断链路异常
- 与 [hccl-test](hccl-test.md) 配合：通过只能证明所测配置与时间窗口，不能据此排除间歇故障；已有证据足够时不重复探测。

## 扩展查阅

- 命令参数或语义本页未覆盖时，按知识访问说明检索固定版本知识卡并核对官方来源。

设备编号与调用限制参考 [Atlas A3 hccn_tool 官方导读](https://www.hiascend.com/doc_center/source/zh/HDK/2610/A3/A3HCCN/hccntool_004.html)；该链接对应 HDK 26.1.x，现场仍以匹配版本的说明为准。
