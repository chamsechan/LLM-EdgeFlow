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
#include "adapter/json_output_parameters.h"
#include "adapter/result_validation.h"
#include "contracts/error_codes.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

constexpr auto kRanked = MakeBlackboardKey<RankedTextBatch>("ranked");

constexpr const char* kOutputSlot = "json_out";

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
  if (!res) return error::kInvalidInput;

  if (!destination) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null output destination", "destination",
        options.Label().c_str());
  }
  const size_t count = destination->count;

  std::vector<const RankedTextBatch::value_type*> first;
  if (!IndexResults(res, destination->count, &first, "ranked_results",
                    options.Label().c_str(), status, true)) {
    return error::kInvalidInput;
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

  return WriteOutputRows<JsonOutputValue>(
      count, options, *destination, written_count, status, kOutputSlot,
      [&](size_t i, JsonOutputValue* out) {
        const auto& cand_list = req_map[static_cast<uint32_t>(i)];
        auto scores = nlohmann::json::array();
        auto indices = nlohmann::json::array();
        for (const auto& candidate : cand_list) {
          scores.push_back(candidate.score);
          indices.push_back(candidate.original_sub_id);
        }
        out->result_json =
            nlohmann::json({{"scores", std::move(scores)},
                            {"sorted_indices", std::move(indices)},
                            {"count", cand_list.size()}})
                .dump();
        return AdapterStatus::Ok();
      });
}

OutputConverterDefinition MakeOperatorRerankResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "cross_rerank";
  def.slot = ExternalOutputSlot<JsonOutputValue>();
  def.logical_ports = {RequiredInputPort(kRanked, "N:1")};
  def.params = JsonOutputParameters();
  def.encode_fn = &EncodeOperatorRerankResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorRerankResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
