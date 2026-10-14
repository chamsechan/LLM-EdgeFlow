#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/json_output_parameters.h"
#include "adapter/output/rule_match_response.h"
#include "adapter/result_validation.h"
#include "core/common_contracts.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

constexpr auto kMatches = MakeBlackboardKey<RuleMatchBatch>("matches");

constexpr const char* kOutputSlot = "json_out";

AdapterStatus EncodeKeyword(const RuleMatchItem& result,
                            JsonOutputValue* output) {
  output->status_code = result.status_code;
  auto response = BuildRuleMatchResponse(result);
  response["is_hit"] = result.is_hit;
  output->result_json = response.dump();
  return AdapterStatus::Ok();
}

int EncodeOperatorKeywordResult(AlgContext* context,
                                const OutputEncodeOptions& options,
                                ExternalOutputBatchView* destination,
                                size_t* written_count, AdapterStatus* status) {
  return EncodeResultRows<JsonOutputValue>(context, options, destination,
                                           written_count, status, kOutputSlot,
                                           kMatches, &EncodeKeyword);
}

OutputConverterDefinition MakeOperatorKeywordResultOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "keyword_match";
  def.slot = ExternalOutputSlot<JsonOutputValue>();
  def.logical_ports = {RequiredInputPort(kMatches)};
  def.params = JsonOutputParameters();
  def.encode_fn = &EncodeOperatorKeywordResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorKeywordResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
