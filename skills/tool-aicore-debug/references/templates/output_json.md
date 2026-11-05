# 输出示例 — JSON

字段、必填条件与边界行为统一见 `references/templates/output_spec.md`。以下示例假设已确认 baseAddr 分配大小至少为 TOTAL_LENGTH 个 float，且 4GiB 偏移超出该分配；保留一个候选与一个修复代码块；多候选或多修复块按相同结构扩展。

```json
{
  "env": {
    "hardware": "Atlas A2",
    "cann": "cann-aicore 8.0.RC1",
    "skill": "tool-aicore-debug@1.0",
    "runtime": "Ascend C++ 6.3.0 / aarch64 / kernel-build 2026.06"
  },
  "candidates": [
    {
      "id": "l2_write_overflow",
      "category": "runtime",
      "fix": {
        "reason": "L2/GM 指针叠加 4GB 超大常量偏移（0x100000000ULL）超出本例已确认的目标分配，违反 offset + write_length <= allocation_bytes 约束；DMA 写入前缺失 ASSERT 守卫。",
        "code_blocks": [
          {
            "title": "移除 4GB 超大常量偏移",
            "language": "cpp",
            "before": "__gm__ float* badGM = reinterpret_cast<__gm__ float*>(\n    reinterpret_cast<uintptr_t>(baseAddr) + 0x100000000ULL);\nGlobalTensor<float> badGMTensor;\nbadGMTensor.SetGlobalBuffer(badGM, TOTAL_LENGTH);\nDataCopy(badGMTensor, xLocal, TOTAL_LENGTH);",
            "after": "__gm__ float* goodGM = reinterpret_cast<__gm__ float*>(baseAddr);\nGlobalTensor<float> goodGMTensor;\ngoodGMTensor.SetGlobalBuffer(goodGM, TOTAL_LENGTH);\nDataCopy(goodGMTensor, xLocal, TOTAL_LENGTH);"
          }
        ]
      }
    }
  ]
}
```
