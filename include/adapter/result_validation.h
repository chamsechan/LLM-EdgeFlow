#pragma once

#include <algorithm>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {

// req_id is the input batch index, never the external request ID. Validate
// every item before exposing an ordered view to either ABI output path.
template <typename Batch>
bool IndexResults(const Batch* batch, const std::vector<uint64_t>* request_ids,
                  std::vector<const typename Batch::value_type*>* ordered,
                  const char* field, const char* biz, AdapterStatus* status,
                  bool ranked = false) {
  auto invalid = [&]() {
    AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing, duplicate or invalid result provenance", field, biz);
    return false;
  };
  if (!batch || !request_ids) return invalid();
  ordered->assign(request_ids->size(), nullptr);
  std::vector<std::vector<uint32_t>> seen(request_ids->size());
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
