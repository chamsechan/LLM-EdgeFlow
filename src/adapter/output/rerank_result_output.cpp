#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

int EncodeOperatorRerankResult(AlgContext* context,
                               const OutputPortBindings& bindings,
                               const OutputEncodeOptions& options,
                               ExternalOutputBatchView* destination,
                               size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* res =
      ReadOutputValue(*context, bindings, kRankedResults, options, status);
  if (!res) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids =
      ReadOutputValue(*context, bindings, kRawRequestIds, options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

  std::vector<const RankedTextBatch::value_type*> first;
  if (!IndexResults(res, raw_req_ids, &first, "ranked_results",
                    options.converter_id.c_str(), status, true)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::unordered_map<uint32_t, std::vector<RankedCandidate>> req_map;
  for (const auto& item : *res) {
    req_map[item.req_id].push_back(item.data);
  }

  for (auto& entry : req_map) {
    auto& list = entry.second;
    std::sort(list.begin(), list.end(),
              [](const auto& a, const auto& b) { return a.rank < b.rank; });
    for (size_t k = 0; k < list.size(); ++k) {
      if (list.size() > 8 || list[k].rank != static_cast<int>(k + 1) ||
          list[k].original_sub_id >= 8) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "Invalid ranked result", "ranked_results",
            options.converter_id.c_str());
      }
    }
  }

  size_t count = raw_req_ids->size();
  if (destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination count is less than output count", "destination",
        options.converter_id.c_str());
  }

  for (size_t i = 0; i < count; ++i) {
    auto* out =
        destination->GetSlot<CompanyOperatorRerankOutput>("rerank_out", i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing rerank_out slot item", "rerank_out",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    out->request_id = (*raw_req_ids)[i];
    const auto& cand_list = req_map[static_cast<uint32_t>(i)];
    int item_cnt = std::min(static_cast<int>(cand_list.size()), 8);
    out->count = item_cnt;
    out->status_code = 0;

    for (int k = 0; k < item_cnt; ++k) {
      out->scores[k] = cand_list[k].score;
      out->sorted_indices[k] = static_cast<int>(cand_list[k].original_sub_id);
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorRerankResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "rerank_result.plain.operator.v1";

  def.schema_id = "rerank_result.plain.response";
  def.external_type = "CompanyOperatorRerankOutput";
  def.max_batch_size = 64;

  def.external_slots = {
      ExternalOutputSlot<CompanyOperatorRerankOutput>("rerank_out")};
  def.logical_ports = {RequiredInputPort(kRawRequestIds),
                       RequiredInputPort(kRankedResults, "N:1")};
  def.encode_fn = &EncodeOperatorRerankResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorRerankResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
