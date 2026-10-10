#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kRanked = MakeBlackboardKey<RankedTextBatch>("ranked");

constexpr const char* kOutputSlot = "rerank_out";

int EncodeOperatorRerankResult(AlgContext* context,
                               const OutputEncodeOptions& options,
                               ExternalOutputBatchView* destination,
                               size_t* written_count, AdapterStatus* status) {
  if (written_count) *written_count = 0;
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  }

  const auto* res = ReadOutputValue(*context, kRanked, options, status);
  if (!res) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids = RequestIds(options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

  std::vector<const RankedTextBatch::value_type*> first;
  if (!IndexResults(res, raw_req_ids, &first, "ranked_results",
                    options.Label().c_str(), status, true)) {
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
      if (list[k].rank != static_cast<int>(k + 1)) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "Invalid ranked result", "ranked_results",
            options.Label().c_str());
      }
    }
  }

  size_t count = raw_req_ids->size();
  if (!destination || destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination count is less than output count", "destination",
        options.Label().c_str());
  }

  size_t written = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!destination->HasSlot(kOutputSlot, i) && !destination->required) {
      continue;
    }
    RerankOutputValue out;

    const auto& cand_list = req_map[static_cast<uint32_t>(i)];
    out.status_code = 0;
    out.items.reserve(cand_list.size());
    for (const auto& candidate : cand_list) {
      out.items.push_back(
          {candidate.score, static_cast<int32_t>(candidate.original_sub_id)});
    }
    if (!WriteOutputValue(*destination, kOutputSlot, i, out, options, status)) {
      return status ? status->Code() : COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
    ++written;
  }

  if (written_count) *written_count = written;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorRerankResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "cross_rerank";
  def.slot = ExternalOutputSlot<RerankOutputValue>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kRanked, "N:1")};
  def.encode_fn = &EncodeOperatorRerankResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorRerankResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
