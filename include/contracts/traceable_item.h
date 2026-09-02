#pragma once

#include <cstdint>
#include <utility>

namespace llm_edgeflow {

/**
 * @brief 具备样本级溯源能力的包装结构体
 *
 * 用于在管线全生命周期中严格追踪样本归属：
 * - 当外部输入 vector<void*> inputs 传入 N 个请求时，第 i 个请求的 req_id = i;
 * - 当第 i 个请求在前处理中被拆分为 M 个分段时，第 j 个分段的 sub_id = j;
 * - 无论经历多少层 Node 传递或固定 Batch 补齐/丢弃，均可通过 req_id 和 sub_id
 * 精确对齐回原输入。
 */
template <typename T>
struct TraceableItem {
  uint32_t req_id = 0;  // 所属外部请求在 inputs 中的索引
  uint32_t sub_id = 0;  // 所属子分片/候选在裂变中的序列索引
  T data;               // 实际业务载荷数据

  TraceableItem() = default;

  TraceableItem(uint32_t r_id, uint32_t s_id, T d)
      : req_id(r_id), sub_id(s_id), data(std::move(d)) {}

  // 便捷转换构造
  template <typename U>
  TraceableItem<U> Map(U new_data) const {
    return TraceableItem<U>(req_id, sub_id, std::move(new_data));
  }
};

}  // namespace llm_edgeflow
