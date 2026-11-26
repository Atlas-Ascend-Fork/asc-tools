# 主机指针属性识别与控制张量（分组 / 页表）主控校验指南

## 一、触发指纹与子例对比

- **典型报错**：`0x800000`（MTE 读写越界）、`0x8000`（标量访存异常）、`507015`，或核内死循环触发 `507014` / `exit_code=124`；或异常用例未按预期返回 `161002`。
- **过/崩子例对比**：设备侧合法非负 `group_list` / `state_block_table` 子例通过；传入 `[-1]` 负数分组、总和或末项超输入总行数 `M`、显式越界块号 `>= block_num`、非法枚举属性（如 `cmpRatio`/`cacheMode`），或传入主机侧缓冲（`host_placed`）时崩溃。
- **30 秒源码搜索词**：在 `op_api/`（`aclnn_*.cpp`）与 `op_host/` 中搜 `groupList`、`group_list`、`BlockTable`、`GetPlacement`、`GetData`、`CommonProcess`、`CheckParams`。

## 二、必查参数与公式清单

1. **先区分主控报错拦截与核内钳制**：从源码已有校验契约、头文件常数及失败输入语义判断：分组长度含负值、前缀和非单调、总和超输入行数 `M`、非法模式属性、以及明确违反前置条件的状态页表，统一在 `op_api` 第一阶段返回 `ACLNN_ERR_PARAM_INVALID`（`161002`）。若属分页注意力或词嵌入梯度在合法调用下需正常过滤越界项，转查 `references/guides/mem_window_overflow_and_index_clamp.md`。
2. **双重判定真实内存位置**：调用方未显式调用 `aclSetTensorPlaceMent` 时，`tensor->GetPlacement()` 可能保持默认值 `0`，不能单独作为主机/设备指针依据，必须同时调用 `aclrtPointerGetAttributes` 核对 `attr.location.type == ACL_MEM_LOCATION_TYPE_DEVICE`。
3. **控制张量元素值域完整约束**：
   - 计数模式（每组行数）：每个元素 `v_i >= 0`，且累加和 `sum(v_i) <= totalRows`（用 `int64_t` 累加防溢出）；
   - 前缀和模式（累积结束行）：`v_0 >= 0`、`v_i >= v_{i-1}`（单调不减），且末项 `v_{G-1} <= totalRows`；
   - 状态块表（主控拦截型）：每个元素 `0 <= id < totalBlocks`，配套压缩比/偏移/缓存模式枚举在文档合法集合内。
4. **主机侧输入张量的设备搬运**：若接口契约允许 `groupList` 等小控制张量位于主机内存，内核启动前必须经框架 `ConvertToTensor` 或第二阶段设备缓冲 `aclrtMemcpy(..., ACL_MEMCPY_HOST_TO_DEVICE)` 下发设备地址，严禁把主机虚拟地址直接写入核函数参数。

## 三、常见误区（避坑）

- **误区 1**：在 `op_api` 直接对设备张量执行 `reinterpret_cast<const int64_t*>(tensor->GetData())[i]`，必触发主控 CPU `SIGSEGV` 段错误。
- **误区 2**：在第一阶段 `aclnn*GetWorkspaceSize` 内调用 `executor->AllocTensor` 试图重绑主机张量，破坏两阶段执行契约导致 `507015` 或空指针崩溃。
- **误区 3**：改 `op_host` 的 `TilingInputsDataDependency` 试图在图模式主控读取设备侧页表，在单算子 `aclnn` 路径下主机侧拿不到设备数据导致校验被跳过。小控制张量直接在 `op_api` 用同步拷回缓冲校验最稳妥。

## 四、标准修复代码模板

```cpp
#include "acl/acl_rt.h"
#include <cstdint>
#include <vector>

static bool ReadSmallIntTensorToHost(const aclTensor* tensor, int64_t elemCnt,
                                     aclDataType dtype, std::vector<int64_t>& out) {
    if (tensor == nullptr || elemCnt < 0 || elemCnt > 65536) return false;
    out.assign(static_cast<size_t>(elemCnt), 0);
    if (elemCnt == 0) return true;
    void* rawPtr = tensor->GetData();
    if (rawPtr == nullptr) return false;

    aclrtPointerAttributes attr{};
    const aclError attrRet = aclrtPointerGetAttributes(rawPtr, &attr);
    const bool isDevice = (attrRet == ACL_SUCCESS &&
                           attr.location.type == ACL_MEM_LOCATION_TYPE_DEVICE);
    const size_t elemBytes = (dtype == ACL_INT32) ? sizeof(int32_t) : sizeof(int64_t);
    std::vector<uint8_t> buf(static_cast<size_t>(elemCnt) * elemBytes);
    if (isDevice) {
        if (aclrtMemcpy(buf.data(), buf.size(), rawPtr, buf.size(),
                        ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
            return false;
        }
    } else {
        std::memcpy(buf.data(), rawPtr, buf.size());
    }
    for (int64_t i = 0; i < elemCnt; ++i) {
        out[i] = (dtype == ACL_INT32)
                     ? static_cast<int64_t>(reinterpret_cast<const int32_t*>(buf.data())[i])
                     : reinterpret_cast<const int64_t*>(buf.data())[i];
    }
    return true;
}

static aclnnStatus ValidateGroupList(const aclTensor* groupList, int64_t groupCnt,
                                     int64_t totalRows, int64_t groupListType) {
    std::vector<int64_t> vals;
    if (!ReadSmallIntTensorToHost(groupList, groupCnt, groupList->GetDataType(), vals)) {
        return ACLNN_ERR_PARAM_INVALID; // 161002
    }
    int64_t prevOrSum = 0;
    for (int64_t v : vals) {
        if (v < 0) return ACLNN_ERR_PARAM_INVALID;
        if (groupListType == 0) { // 前缀和模式
            if (v < prevOrSum || v > totalRows) return ACLNN_ERR_PARAM_INVALID;
            prevOrSum = v;
        } else {                  // 计数模式
            if (v > totalRows - prevOrSum) return ACLNN_ERR_PARAM_INVALID;
            prevOrSum += v;
        }
    }
    return ACLNN_SUCCESS;
}
```
