#pragma once

#include <algorithm>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {

// req_id 是输入批内索引，而非外部请求 ID。向任一 ABI 输出路径暴露有序
// 视图前，须校验每个条目。
template <typename Batch>
bool IndexResults(const Batch* batch, size_t request_count,
                  std::vector<const typename Batch::value_type*>* ordered,
                  const char* field, const char* biz, AdapterStatus* status,
                  bool ranked = false) {
  auto invalid = [&]() {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing, duplicate or invalid result provenance", field, biz);
    return false;
  };
  if (!batch) return invalid();
  ordered->assign(request_count, nullptr);
  std::vector<std::vector<uint32_t>> seen(request_count);
  for (const auto& item : *batch) {
    if (item.req_id >= ordered->size() || (!ranked && item.sub_id != 0)) {
      return invalid();
    }
    auto& ids = seen[item.req_id];
    if (std::find(ids.begin(), ids.end(), item.sub_id) != ids.end())
      return invalid();
    ids.push_back(item.sub_id);
    if (item.sub_id == 0) (*ordered)[item.req_id] = &item;
  }
  for (const auto* item : *ordered)
    if (!item) return invalid();
  return true;
}

inline bool IsSuccessfulDocument(const JsonDocumentItem& item) {
  return item.is_valid && item.parse_status != JsonParseStatus::kFailed &&
         item.parse_status != JsonParseStatus::kFallbackApplied;
}

}  // namespace llm_edgeflow
