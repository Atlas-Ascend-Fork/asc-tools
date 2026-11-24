# hccl_test — HCCL 通信基准测试

> 在所选集合类型、数据规模、设备集合与当前时间窗口内剥离业务做通信基准验证；通过仅说明该测例配置下通信链路与环境可正常工作，失败需先核对 MPI、环境变量与设备映射，再缩小到链路与硬件范围。

## 工具定位

CANN 自带 HCCL 通信基准测试套件（`$ASCEND_HOME_PATH/tools/hccl_test/`），基于 MPI+HCCL，对各 collective 做带宽与正确性测试。也用于新环境/重装后通信冒烟。

**device id 约定**：`HCCL_TEST_USE_DEVS` 用 **chip id**（4,6），`-p` 指每节点 NPU 数。

## 调用命令

```bash
# 1. 优先复用现场环境或用户授权目录中已有的 hccl_test 可执行文件（显式设置 HCCL_TEST_BIN_DIR）
HCCL_TEST_BIN_DIR="${HCCL_TEST_BIN_DIR:-}"
if [ -n "$HCCL_TEST_BIN_DIR" ]; then
  case "$HCCL_TEST_BIN_DIR" in
    /*) ;;
    *) echo "require absolute HCCL_TEST_BIN_DIR" >&2; exit 1 ;;
  esac
  [ -d "$HCCL_TEST_BIN_DIR" ] && [ -x "$HCCL_TEST_BIN_DIR/all_reduce_test" ] || {
    echo "missing executable $HCCL_TEST_BIN_DIR/all_reduce_test" >&2
    exit 1
  }
else
  # 2. 若确需构建，先规范化并校验 ASCEND_HOME_PATH、AUTHORIZED_BUILD_ROOT、HCCL_TEST_WORKDIR 与 MPI_HOME，禁止写入 SDK 安装树或覆盖既有路径
  case "${ASCEND_HOME_PATH:-}:${AUTHORIZED_BUILD_ROOT:-}:${HCCL_TEST_WORKDIR:-}:${MPI_HOME:-}" in
    /*:/*:/*:/*) ;;
    *) echo "require non-empty absolute ASCEND_HOME_PATH, AUTHORIZED_BUILD_ROOT, HCCL_TEST_WORKDIR, MPI_HOME" >&2; exit 1 ;;
  esac
  SDK_REAL=$(realpath -m -- "$ASCEND_HOME_PATH") || exit 1
  SRC_REAL=$(realpath -m -- "$SDK_REAL/tools/hccl_test") || exit 1
  AUTH_REAL=$(realpath -m -- "$AUTHORIZED_BUILD_ROOT") || exit 1
  WORK_REAL=$(realpath -m -- "$HCCL_TEST_WORKDIR") || exit 1
  MPI_REAL=$(realpath -m -- "$MPI_HOME") || exit 1
  case "$SDK_REAL" in "/"|"") echo "invalid SDK_REAL" >&2; exit 1 ;; esac
  case "$AUTH_REAL" in "/"|"") echo "invalid AUTH_REAL" >&2; exit 1 ;; esac
  case "$WORK_REAL" in "/"|"") echo "invalid WORK_REAL" >&2; exit 1 ;; esac
  case "$MPI_REAL" in "/"|"") echo "invalid MPI_REAL" >&2; exit 1 ;; esac
  [ -d "$SRC_REAL" ] && [ -d "$MPI_REAL" ] && [ -d "$AUTH_REAL" ] || {
    echo "invalid source, MPI, or authorized root directory" >&2
    exit 1
  }
  case "$WORK_REAL/" in
    "$AUTH_REAL/"?*) ;;
    *) echo "HCCL_TEST_WORKDIR must be a dedicated subdirectory inside AUTHORIZED_BUILD_ROOT" >&2; exit 1 ;;
  esac
  case "$WORK_REAL/" in
    "$SDK_REAL/"*|"$SRC_REAL/"*) echo "HCCL_TEST_WORKDIR must be outside SDK tree" >&2; exit 1 ;;
  esac
  case "$SDK_REAL/" in
    "$WORK_REAL/"*) echo "HCCL_TEST_WORKDIR must not contain SDK tree" >&2; exit 1 ;;
  esac
  [ ! -e "$WORK_REAL" ] || {
    echo "refuse to overwrite existing HCCL_TEST_WORKDIR: $WORK_REAL" >&2
    exit 1
  }
  mkdir -p -- "$WORK_REAL" || exit 1
  cp -a -- "${SRC_REAL}/." "${WORK_REAL}/" || exit 1
  ( cd -- "$WORK_REAL" && make MPI_HOME="$MPI_REAL" ASCEND_DIR="$SDK_REAL" ) || exit 1
  HCCL_TEST_BIN_DIR="${WORK_REAL}/bin"
fi
[ -x "${HCCL_TEST_BIN_DIR}/all_reduce_test" ] || {
  echo "hccl_test binary not ready in ${HCCL_TEST_BIN_DIR}" >&2
  exit 1
}

# 授权工作目录下的 run.sh 示例（指定 chip 4,6）：
#   export HCCL_TEST_USE_DEVS="4,6"
#   exec "$@"

[ -x ./run.sh ] || { echo "missing executable ./run.sh" >&2; exit 1; }
for bin in all_reduce_test all_gather_test alltoall_test; do
  [ -x "${HCCL_TEST_BIN_DIR}/${bin}" ] || { echo "missing executable ${HCCL_TEST_BIN_DIR}/${bin}" >&2; exit 1; }
done

# allreduce 带宽 + 正确性校验（-c 1）
mpirun -n 2 ./run.sh "${HCCL_TEST_BIN_DIR}/all_reduce_test" -b 8K -e 64M -f 2 -p 2 -d fp32 -o sum -c 1 -n 20 || exit 1
# allgather
mpirun -n 2 ./run.sh "${HCCL_TEST_BIN_DIR}/all_gather_test" -b 8K -e 64M -f 2 -p 2 || exit 1
# alltoall
mpirun -n 2 ./run.sh "${HCCL_TEST_BIN_DIR}/alltoall_test" -b 8K -e 64M -f 2 -p 2 || exit 1
# 多机（hostfile 列节点）
mpirun -f hostfile -n 16 "${HCCL_TEST_BIN_DIR}/all_reduce_test" -b 8K -e 64M -f 2 -p 8 || exit 1
```

**关键参数**：
| 参数 | 说明 |
|------|------|
| `-p, --npus <n>` | 每节点 NPU 数 |
| `-b, -e, -f` | 起始/结束/步长数据量（`-b 8K -e 64M -f 2` 按 2 倍递增） |
| `-d, --datatype` | 数据类型（fp32/fp16/int8 等） |
| `-o, --op` | 归约类型（sum/prod/max/min） |
| `-c, --check 0/1` | 结果正确性校验（默认 1 开启） |
| `-n, --iters <n>` | 迭代次数（默认 20） |
| `-w, --warmup_iters <n>` | 预热（默认 10） |

**支持的可执行**（11 个）：`all_reduce_test`、`all_gather_test`、`all_gatherv_test`、`alltoall_test`、`alltoallv_test`、`alltoallvc_test`、`broadcast_test`、`reduce_test`、`reduce_scatter_test`、`reduce_scatterv_test`、`scatter_test`。

## 输出解读

- `-c 1`（默认开启）：测试结束打印 `correct: yes/no`，no 即数据错乱
- 带宽数据：各数据点（8K~64M）的 algbw/busbw，对比官方基线判定是否达标

## 诊断决策

| hccl_test 输出 | 指向 | 详见 |
|---------------|------|------|
| 通过（`correct: yes` + 带宽达标） | 当前所测配置与时间窗口内未见链路异常；结合业务规模、调用路径及故障时段日志核查业务/算子侧或间歇链路问题 → [comm-operator-anomaly](../symptoms/comm-operator-anomaly.md) / [collective-param-mismatch](../symptoms/collective-param-mismatch.md) | [hccn-tool](hccn-tool.md) |
| 失败（`correct: no` 或超时） | 先核对 MPI 配置、环境变量与 `HCCL_TEST_USE_DEVS` 设备映射；排除配置因素后触发 [hccn-tool](hccn-tool.md) + [msnpureport](msnpureport.md) 缩窄链路或硬件问题 → [communication-timeout](../symptoms/communication-timeout.md) | |
| 带宽不达预期 | 链路带宽瓶颈，配合 [hccn-tool](hccn-tool.md) `-bandwidth -g` 与 `-roce_test ib_read_bw` | |

**诊断价值**：与业务用同一 collective 类型测试最具价值（业务 allreduce 异常就测 `all_reduce_test`）。

## 降级策略

- 未 make 构建或 MPI 不可用：`tool_evidence` 标注 `skipped: hccl_test not built / MPI unavailable`
- 无 MPI 多机环境：单机内 `mpirun -n 2`（cross-card）仍可验证片间链路
- 与 [hccn-tool](hccn-tool.md) 配合：hccl_test 仅覆盖所测 collective、规模、设备与时间窗口，若原故障时段日志出现 `error cqe`/链路告警仍需查 hccn_tool；hccl_test 不可用时直接用 hccn_tool

## 扩展查阅

- 命令参数或语义本页未覆盖时，按知识访问说明检索固定版本知识卡并核对官方来源。
